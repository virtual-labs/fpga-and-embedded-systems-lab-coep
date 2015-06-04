
/* 	
  	Odin - RTL Verilog synthesis back-end to FPGAs
	Copyright (C) 2005  Peter Jamieson

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

	Contact: jamieson.peter@gmail.com
*/

void olap_add_node_to_level_list(node_t *node, int level);
void olap_levelizer();
void olap_visit_output_pins_of_node_to_levelize(node_t* node, int current_level, short current_propogation_value);
void olap_recursive_leveler(node_t *node, int input_nodes_level, short propogation_value, int input_pin_index);
