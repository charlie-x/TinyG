/*
 * plan_arc.c - arc planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2013 Alden S. Hart, Jr.
 * Portions copyright (c) 2009 Simen Svale Skogsrud
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* This module actually contains some parts that belong ion the canonical machine, 
 * and other parts that belong at the motion planner level, but the whole thing is 
 * treated as if it were part of the motion planner.
 */

#include "tinyg.h"
#include "config.h"
#include "canonical_machine.h"
#include "plan_arc.h"
#include "planner.h"
#include "util.h"

#ifdef __cplusplus
extern "C"{
#endif

// Allocate arc planner singleton structure

arc_t arc;


// Local functions

static stat_t _compute_arc(void);
static stat_t _compute_arc_offsets_from_radius(void);
static float _get_arc_time (const float linear_travel, const float angular_travel, const float radius);
static float _get_theta(const float x, const float y);
static stat_t _test_arc_soft_limits(void);

/*****************************************************************************
 * Canonical Machining arc functions (arc prep for planning and runtime)
 *
 * cm_arc_init()	 - initialize arcs
 * cm_arc_feed() 	 - canonical machine entry point for arc
 * cm_arc_callback() - mail-loop callback for arc generation
 * cm_abort_arc()	 - stop an arc in process
 */

/*
 * cm_arc_init() - initialize arc structures
 */
void cm_arc_init()
{
	arc.magic_start = MAGICNUM;
	arc.magic_end = MAGICNUM;
}

/*
 * cm_arc_feed() - canonical machine entry point for arc
 *
 * Generates an arc by queueing line segments to the move buffer. The arc is 
 * approximated by generating a large number of tiny, linear segments.
 */
stat_t cm_arc_feed(float target[], float flags[],// arc endpoints
				   float i, float j, float k, 	 // raw arc offsets
				   float radius, 				 // non-zero radius implies radius mode
				   uint8_t motion_mode)			 // defined motion mode
{
	// trap zero feed rate condition
	if ((gm.inverse_feed_rate_mode == false) && (fp_ZERO(gm.feed_rate))) {
		return (STAT_GCODE_FEEDRATE_ERROR);
	}
	// Trap conditions where no arc movement will occur, but the system is still in 
	// arc motion mode - this is not an error. This can happen when a F word or M 
	// word is by itself.(The tests below are organized for execution efficiency)
	if ( fp_ZERO(i) && fp_ZERO(j) && fp_ZERO(k) && fp_ZERO(radius) ) {
		if ( fp_ZERO((flags[AXIS_X] + flags[AXIS_Y] + flags[AXIS_Z] + 
					  flags[AXIS_A] + flags[AXIS_B] + flags[AXIS_C]))) {
			return (STAT_OK);
		}
	}

	// set values in the Gcode model state & copy it (linenum was already captured)
	cm_set_model_target(target, flags);
	gm.motion_mode = motion_mode;
	cm_set_work_offsets(&gm);					// capture the fully resolved offsets to gm
	memcpy(&arc.gm, &gm, sizeof(GCodeState_t)); // copy GCode context to arc singleton - some will be overwritten to run segments

	// populate the arc control singleton
//	copy_axis_vector(arc.endpoint, gm.target);	// +++++ Diagnostic - save target position

	copy_axis_vector(arc.position, gmx.position);// set initial arc position from gcode model
	arc.radius = _to_millimeters(radius);		// set arc radius or zero
	arc.offset[0] = _to_millimeters(i);			// copy offsets with conversion to canonical form (mm)
	arc.offset[1] = _to_millimeters(j);
	arc.offset[2] = _to_millimeters(k);

	// Set the arc plane for the current G17/G18/G19 setting 
	// Plane axis 0 and 1 are the arc plane, 2 is the linear axis normal to the arc plane 
	if (gm.select_plane == CANON_PLANE_XY) {	// G17 - the vast majority of arcs are in the G17 (XY) plane
		arc.plane_axis_0 = AXIS_X;		
		arc.plane_axis_1 = AXIS_Y;
		arc.plane_axis_2 = AXIS_Z;
	} else if (gm.select_plane == CANON_PLANE_XZ) {	// G18
		arc.plane_axis_0 = AXIS_X;		
		arc.plane_axis_1 = AXIS_Z;
		arc.plane_axis_2 = AXIS_Y;
	} else if (gm.select_plane == CANON_PLANE_YZ) {	// G19
		arc.plane_axis_0 = AXIS_Y;
		arc.plane_axis_1 = AXIS_Z;
		arc.plane_axis_2 = AXIS_X;
	}

	// compute arc runtime values and prep for execution by the callback
	ritorno(_compute_arc());
	ritorno(_test_arc_soft_limits());			// test if arc will trip soft limits
	cm_cycle_start();							// if not already started
	arc.run_state = MOVE_STATE_RUN;				// enable arc to be run from the callback
	cm_conditional_set_model_position(STAT_OK);	// set endpoint position if the arc was successful
	return (STAT_OK);
}

/*
 * cm_arc_callback() - generate an arc
 *
 *	cm_arc_callback() is called from the controller main loop. Each time it's called it 
 *	queues as many arc segments (lines) as it can before it blocks, then returns.
 *
 *  Parts of this routine were originally sourced from the grbl project.
 */

stat_t cm_arc_callback() 
{
	if (arc.run_state == MOVE_STATE_OFF) { return (STAT_NOOP);}
	if (mp_get_planner_buffers_available() < PLANNER_BUFFER_HEADROOM) { return (STAT_EAGAIN);}

	arc.theta += arc.segment_theta;
	arc.gm.target[arc.plane_axis_0] = arc.center_0 + sin(arc.theta) * arc.radius;
	arc.gm.target[arc.plane_axis_1] = arc.center_1 + cos(arc.theta) * arc.radius;
	arc.gm.target[arc.plane_axis_2] += arc.segment_linear_travel;
	mp_aline(&arc.gm);								// run the line
	copy_axis_vector(arc.position, arc.gm.target);	// update arc current position	

	if (--arc.segment_count > 0) return (STAT_EAGAIN);
	arc.run_state = MOVE_STATE_OFF;
	return (STAT_OK);
}

/*
 * cm_abort_arc() - stop arc movement without maintaining position
 *
 *	OK to call if no arc is running
 */

void cm_abort_arc() 
{
	arc.run_state = MOVE_STATE_OFF;
}

/*
 * _compute_arc() - compute arc from I and J (arc center point)
 *
 *	The theta calculation sets up an clockwise or counterclockwise arc from the current 
 *	position to the target position around the center designated by the offset vector. 
 *	All theta-values measured in radians of deviance from the positive y-axis. 
 *
 *                      | <- theta == 0
 *                    * * *
 *                  *       *
 *                *           *
 *                *     O ----T   <- theta_end (e.g. 90 degrees: theta_end == PI/2)
 *                *   /
 *                  C   <- theta_start (e.g. -145 degrees: theta_start == -PI*(3/4))
 *
 *  Parts of this routine were originally sourced from the grbl project.
 */
static stat_t _compute_arc()
{

	// A non-zero radius value indicated a radius arc
	// Compute IJK offset coordinates. These override any current IJK offsets
	if (fp_NOT_ZERO(arc.radius)) ritorno(_compute_arc_offsets_from_radius()); // returns if error

	// Calculate the theta (angle) of the current point (see header notes)
	// Arc.theta is starting point for theta (theta_start)
	arc.theta = _get_theta(-arc.offset[arc.plane_axis_0], -arc.offset[arc.plane_axis_1]);
	if(isnan(arc.theta) == true) return(STAT_ARC_SPECIFICATION_ERROR);

	// calculate the theta (angle) of the target point
	float theta_end = _get_theta(
		arc.gm.target[arc.plane_axis_0] - arc.offset[arc.plane_axis_0] - arc.position[arc.plane_axis_0], 
 		arc.gm.target[arc.plane_axis_1] - arc.offset[arc.plane_axis_1] - arc.position[arc.plane_axis_1]);
	if(isnan(theta_end) == true) return (STAT_ARC_SPECIFICATION_ERROR);

	// ensure that the difference is positive so we have clockwise travel
	if (theta_end < arc.theta) { theta_end += 2*M_PI; }

	// compute angular travel and invert if gcode wants a counterclockwise arc
	// if angular travel is zero interpret it as a full circle
	arc.angular_travel = theta_end - arc.theta;
	if (fp_ZERO(arc.angular_travel)) {
		if (gm.motion_mode == MOTION_MODE_CCW_ARC) {
			arc.angular_travel -= 2*M_PI;
		} else {
			arc.angular_travel = 2*M_PI;
		}
	} else {
		if (gm.motion_mode == MOTION_MODE_CCW_ARC) {
			arc.angular_travel -= 2*M_PI;
		}
	}

	// Find the radius, calculate travel in the depth axis of the helix,
	// and compute the time it should take to perform the move
	arc.radius = hypot(arc.offset[arc.plane_axis_0], arc.offset[arc.plane_axis_1]);
	arc.linear_travel = arc.gm.target[arc.plane_axis_2] - arc.position[arc.plane_axis_2];

	// length is the total mm of travel of the helix (or just a planar arc)
	arc.length = hypot(arc.angular_travel * arc.radius, fabs(arc.linear_travel));
	if (arc.length < cm.arc_segment_len) return (STAT_MINIMUM_LENGTH_MOVE_ERROR); // too short to draw

	arc.time = _get_arc_time(arc.linear_travel, arc.angular_travel, arc.radius);

	// Find the minimum number of segments that meets these constraints...
	float segments_required_for_chordal_accuracy = arc.length / sqrt(4*cm.chordal_tolerance * (2 * arc.radius - cm.chordal_tolerance));
	float segments_required_for_minimum_distance = arc.length / cm.arc_segment_len;
	float segments_required_for_minimum_time = arc.time * MICROSECONDS_PER_MINUTE / MIN_ARC_SEGMENT_USEC;
	arc.segments = floor(min3(segments_required_for_chordal_accuracy,
							   segments_required_for_minimum_distance,
							   segments_required_for_minimum_time));

	arc.segments = max(arc.segments, 1);		//...but is at least 1 segment
	arc.gm.move_time = arc.time / arc.segments;	// gcode state struct gets segment_time, not arc time
	arc.segment_count = (int32_t)arc.segments;
	arc.segment_theta = arc.angular_travel / arc.segments;
	arc.segment_linear_travel = arc.linear_travel / arc.segments;
	arc.center_0 = arc.position[arc.plane_axis_0] - sin(arc.theta) * arc.radius;
	arc.center_1 = arc.position[arc.plane_axis_1] - cos(arc.theta) * arc.radius;
	arc.gm.target[arc.plane_axis_2] = arc.position[arc.plane_axis_2];	// initialize the linear target
	return (STAT_OK);
}

/* 
 * _compute_arc_offsets_from_radius() - compute arc center (offset) from radius. 
 *
 *  Needs to calculate the center of the circle that has the designated radius and 
 *	passes through both the current position and the target position
 *		  
 *	This method calculates the following set of equations where:
 *	`  [x,y] is the vector from current to target position, 
 *		d == magnitude of that vector, 
 *		h == hypotenuse of the triangle formed by the radius of the circle, 
 *			 the distance to the center of the travel vector. 
 *		  
 *	A vector perpendicular to the travel vector [-y,x] is scaled to the length
 *	of h [-y/d*h, x/d*h] and added to the center of the travel vector [x/2,y/2]
 *	to form the new point [i,j] at [x/2-y/d*h, y/2+x/d*h] which will be the 
 *	center of the arc.
 *        
 *		d^2 == x^2 + y^2
 *		h^2 == r^2 - (d/2)^2
 *		i == x/2 - y/d*h
 *		j == y/2 + x/d*h
 *                                        O <- [i,j]
 *                                     -  |
 *                           r      -     |
 *                               -        |
 *                            -           | h
 *                         -              |
 *           [0,0] ->  C -----------------+--------------- T  <- [x,y]
 *                     | <------ d/2 ---->|
 *                  
 *		C - Current position
 *		T - Target position
 *		O - center of circle that pass through both C and T
 *		d - distance from C to T
 *		r - designated radius
 *		h - distance from center of CT to O
 *  
 *	Expanding the equations:
 *		d -> sqrt(x^2 + y^2)
 *		h -> sqrt(4 * r^2 - x^2 - y^2)/2
 *		i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2 
 *		j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2
 * 
 *	Which can be written:  
 *		i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
 *		j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
 *  
 *	Which we for size and speed reasons optimize to:
 *		h_x2_div_d = sqrt(4 * r^2 - x^2 - y^2)/sqrt(x^2 + y^2)
 *		i = (x - (y * h_x2_div_d))/2
 *		j = (y + (x * h_x2_div_d))/2
 *
 * ----Computing clockwise vs counter-clockwise motion ----
 *
 *	The counter clockwise circle lies to the left of the target direction. 
 *	When offset is positive the left hand circle will be generated - 
 *	when it is negative the right hand circle is generated.
 *
 *                                   T  <-- Target position
 *  
 *                                   ^ 
 *      Clockwise circles with       |     Clockwise circles with
 *		this center will have        |     this center will have
 *      > 180 deg of angular travel  |     < 180 deg of angular travel, 
 *                        \          |      which is a good thing!
 *                         \         |         /
 *  center of arc when  ->  x <----- | -----> x <- center of arc when 
 *  h_x2_div_d is positive           |             h_x2_div_d is negative
 *                                   |
 *                                   C  <-- Current position
 *
 *
 *	Assumes arc singleton has been pre-loaded with target and position.
 *	Parts of this routine were originally sourced from the grbl project.
 */
static stat_t _compute_arc_offsets_from_radius()
{
	// Calculate the change in position along each selected axis
	float x = gm.target[arc.plane_axis_0]-gmx.position[arc.plane_axis_0];
	float y = gm.target[arc.plane_axis_1]-gmx.position[arc.plane_axis_1];

	// == -(h * 2 / d)
	float h_x2_div_d = -sqrt(4 * square(arc.arc_radius) - (square(x) - square(y))) / hypot(x,y);

	// If r is smaller than d the arc is now traversing the complex plane beyond
	// the reach of any real CNC, and thus - for practical reasons - we will 
	// terminate promptly
	if(isnan(h_x2_div_d) == true) { return (STAT_FLOATING_POINT_ERROR);}

	// Invert the sign of h_x2_div_d if circle is counter clockwise (see header notes)
	if (gm.motion_mode == MOTION_MODE_CCW_ARC) { h_x2_div_d = -h_x2_div_d;}

	// Negative R is g-code-alese for "I want a circle with more than 180 degrees
	// of travel" (go figure!), even though it is advised against ever generating
	// such circles in a single line of g-code. By inverting the sign of 
	// h_x2_div_d the center of the circles is placed on the opposite side of 
	// the line of travel and thus we get the unadvisably long arcs as prescribed.
	if (arc.arc_radius < 0) { h_x2_div_d = -h_x2_div_d; }

	// Complete the operation by calculating the actual center of the arc
	arc.offset[arc.plane_axis_0] = (x-(y*h_x2_div_d))/2;
	arc.offset[arc.plane_axis_1] = (y+(x*h_x2_div_d))/2;
	arc.offset[arc.plane_axis_2] = 0;
	return (STAT_OK);
}

/*
 * _get_arc_time ()
 *
 *	This is a naiive rate-limiting function. The arc drawing time is computed not 
 *	to exceed the time taken in the slowest dimension - in the arc plane or in 
 *	linear travel. Maximum feed rates are compared in each dimension, but the 
 *	comparison assumes that the arc will have at least one segment where the unit 
 *	vector is 1 in that dimension. This is not true for any arbitrary arc, with 
 *	the result that the time returned may be less than optimal.
 *
 *	Room for improvement: At least take the hypotenuse of the planar movement and
 *	the linear travel into account, but how many people actually use helixes?
 */
static float _get_arc_time (const float linear_travel,	// in mm
							const float angular_travel,	// in radians
							const float radius)			// in mm
{
	float tmp;
	float move_time=0;	// picks through the times and retains the slowest
	float planar_travel = fabs(angular_travel * radius);// travel in arc plane

	if (gm.inverse_feed_rate_mode == true) {
		move_time = gmx.inverse_feed_rate;
	} else {
		move_time = sqrt(square(planar_travel) + square(linear_travel)) / gm.feed_rate;
	}
	if ((tmp = planar_travel/cm.a[arc.plane_axis_0].feedrate_max) > move_time) {
		move_time = tmp;
	}
	if ((tmp = planar_travel/cm.a[arc.plane_axis_1].feedrate_max) > move_time) {
		move_time = tmp;
	}
	if ((tmp = fabs(linear_travel/cm.a[arc.plane_axis_2].feedrate_max)) > move_time) {
		move_time = tmp;
	}
	return (move_time);
}

/* 
 * _get_theta(float x, float y)
 *
 *	Find the angle in radians of deviance from the positive y axis
 *	negative angles to the left of y-axis, positive to the right.
 */

static float _get_theta(const float x, const float y)
{
	float theta = atan(x/fabs(y));

	if (y>0) {
		return (theta);
	} else {
		if (theta>0) {
			return ( M_PI-theta);
    	} else {
			return (-M_PI-theta);
		}
	}
}


/* 
 * _test_arc_soft_limits() - return error code if soft limit is exceeded
 *
 *	Must be called with endpoint target in arc.gm struct
 */
static stat_t _test_arc_soft_limits()
{
	return (cm_test_soft_limits(arc.gm.target));
}

//##########################################
//############## UNIT TESTS ################
//##########################################

#ifdef __UNIT_TESTS
#ifdef __UNIT_TEST_PLANNER

void mp_plan_arc_unit_tests()
{
//	_mp_test_buffers();
}

#endif
#endif

#ifdef __cplusplus
}
#endif
