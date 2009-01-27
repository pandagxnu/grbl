/*
  main.c - An embedded CNC Controller with rs274/ngc (g-code) support
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <avr/io.h>
#include <avr/sleep.h>
#include "motion_control.h"
#include "gcode.h"
#include "spindle_control.h"
#include "serial_protocol.h"

int main(void)
{
  mc_init(); // initialize motion control subsystem
  gc_init(); // initialize gcode-parser
  spindle_init(); // initialize spindle controller
  sp_init(); // initialize the serial protocol
  
  gc_execute_line("123.1");  
  for(;;){
    sleep_mode();
    sp_process(); // process the serial protocol
  }
  return 0;   /* never reached */
}