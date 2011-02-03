/*
 * main.c - TinyG - An embedded CNC controller with rs274/ngc (g-code) support
 * Part of TinyG project
 *
 * Copyright (c) 2010 Alden S. Hart, Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 */
/*

---- In order to compile and link in AVRstudio you should do this... ----

	Device should have already been selected to be atxmega256a3. If not:
		In AVRstudio select Project / Configuration Options
		In main window select device atxmega256a3

	Configure clock frequency (optional, but recommended)
		In Project / Configuration Options main window:
		Frequency should be 32000000 		(32 Mhz)
	  also may want set 32.0000 Mhz in Simulator2 configs:
		Go into debug mode
		In Debug / AVR Simulator 2 Options
		Set clock frequency to 32 Mhz.

	Add libm.a (math lib) otherwise the floating point will fail.
		In AVRstudio select Project / Configuration Options
		Select Libraries
		Move libm.a from the left pane to the right pane
		ref: http://www.avrfreaks.net/index.php?name=PNphpBB2&file=printview&t=80040&start=0

	Add floating point formatting code to the linker string (for printf %f to work)
		In AVRstudio select Project / Configuration Options
		Select Custom Options
		In the left pane (Custom Compilation Options) Select [Linker Options] 
		Add the following lines to the right pane (is now linker options)
			-Wl,-u,vfprintf				(thats: W"ell" not W"one")
			-lprintf_flt
			-lm
		ref: http://www.avrfreaks.net/index.php?name=PNphpBB2&file=printview&t=92299&start=0
		ref: http://www.cs.mun.ca/~paul/cs4723/material/atmel/avr-libc-user-manual-1.6.5/group__avr__stdio.html#ga3b98c0d17b35642c0f3e4649092b9f1

  	An annoying avr20100110 bug: 
  		If you are running WinAVR-20100110 you may be asked to locate libraries or 
		include files that were known to a previous avr-gcc version. 

		When asked to browse for stdlib files, go to: 	 C:\WinAVR-20100110\avr\lib\avrxmega6
									(or is it: C:\WinAVR-20100110\lib\gcc\avr\4.3.3\avrxmega6

		When asked to browse for include files go to: C:\WinAVR-20100110\avr\include

---- Using "screen" on Mac OSX to drive it ----

  Procedure to use the USB port from mac OSX:
	- Install the FTDI virtual serial port driver for your OS.
	- Find your tty device in /dev directory, e.g.
		/dev/tty.usbserial-A700eUQo
	- Invoke screen using your tty device at 115200 baud. From terminal prompt, e.g:
		screen /dev/tty.usbserial-A700eUQo 115200

  If you are running screen (under terminal) in OSX you may want to do this first:
	in terminal, enter: "defaults write com.apple.Terminal TermCapString xterm"
						"export TERM=xterm"
  (ref: http://atomized.org/2006/05/fixing-backspace-in-screen-in-terminal-on-os-x/)

---- Coding conventions ----

  Adopted the following xmega C variable naming conventions
  (See AVR1000: Getting Started Writing C-code for XMEGA [doc8075.pdf] )

	varname_bm		- single bit mask, e.g. 0x40 aka (1<<4)
	varname_bp		- single bit position, e.g. 4 for the above example
	varname_gm		- group bit mask, e.g. 0x0F
	varname_gc		- group configuration, e.g. 0x0A is 2 bits in the above _gm

  These conventions are used for internal variables but may be relaxed for old 
  UNIX vars and DEFINES that don't follow these conventions.

---- Notes on comments ----

. This code is possibly over-commented. I do this to remind 
  myself in 6 months on what I was thinking when I wrote the code. 
  More skilled programmers won't need all these comments. - Alden

---- ToDo ----

	- implement Aux device for direct Arduino drive
	- put arc draw endpoint back in
	- implement RS485 device and packet protocol
	- finish direct drive commands
*/

#include <stdio.h>			// defines "FILE"
#include <avr/interrupt.h>
#include <util/delay.h>

#include "system.h"
#include "xmega_init.h"
#include "xmega_interrupts.h"
#include "xmega_eeprom.h"
#include "xmega_rtc.h"
#include "xio.h"

#include "tinyg.h"
#include "settings.h"
#include "controller.h"
#include "network.h"
#include "signals.h"
#include "config.h"
#include "stepper.h"
#include "limit_switches.h"
#include "motor_queue.h"
#include "motion_control.h"
#include "spindle.h"
#include "direct_drive.h"
#include "encoder.h"
#include "gcode.h"

/*
 * Init structure
 *
 *	System startup procedes through the following levels:
 *
 *	  tg_system_init() 			- called first and only once
 *	  tg_application_init()		- typically only called at startup
 *	  tg_unit_tests() 			- called at startup only if unit tests enabled
 *	  tg_application_startup()	- called last; may be called again at any point
 *
 * 	The first three are managed in main.c
 *
 *	tg_application_startup() is provided by controller.c. It is used for 
 *	application starts and restarts (like for limit switches). It manages 
 *	power-on actions like homing cycles and any pre-loaded commands to the 
 *	input buffer.
 */

void tg_system_init(void)
{
	cli();					// These inits are order dependent:
	hw_init();				// (1) hardware setup
	xio_init();				// (2) xmega io subsystem
	cfg_init();				// (3) get config record from eeprom (reqs xio)
	tg_init();				// (4) tinyg controller (selects std devices)
	xio_init_stdio();		// (5) set stdin, stdout, stderr
	sig_init();				// (6) signal flags
	rtc_init();				// (7) real time counter
}

void tg_application_init(void) 
{
	st_init(); 				// stepper subsystem
	ls_init();				// limit switches
	mq_init();				// move buffers
	mc_init();				// motion control subsystem
	sp_init();				// spindle controller
	en_init();				// encoders
	gc_init();				// gcode-parser
	dd_init();				// direct drive commands

	PMIC_SetVectorLocationToApplication();  // as opposed to boot rom
	PMIC_EnableLowLevel();	// enable TX interrupts
	PMIC_EnableMediumLevel();//enable RX interrupts
	PMIC_EnableHighLevel();	// enable stepper timer interrupts
	sei();					// enable global interrupts

	tg_alive();				// (LAST) announce things are online
}

void tg_unit_tests(void)
{
  #ifdef __UNIT_TESTS
//	xio_tests();				// IO subsystem
//	EEPROM_tests();				// EEPROM functions
//	cfg_tests();				// config functions
	mc_unit_tests();			// motion control module
  #endif
}

/*
 * MAIN
 */

int main(void)
{
	tg_system_init();
	tg_application_init();
	tg_unit_tests();
	tg_application_startup();

#ifdef __NORMAL_MODE
	for(;;){
		tg_controller();// this mode executes gcode blocks received via USB
	}
#endif

#ifdef __RELAY_MODE
	for(;;){
		tg_repeater();	// this mode receives on USB and repeats to RS485
	}
#endif

#ifdef __SLAVE_MODE
	for(;;){
		tg_receiver();	// this mode executes gcode blocks received via RS485
	}
#endif
}