/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ---
 *
 *  This is the AST frontend library.
 *
 *  The AST frontend library is not a frontend on it's own but provides a
 *  generic abstract syntax tree (AST) abstraction for HDL code and can be
 *  used by HDL frontends. See "ast.h" for an overview of the API and the
 *  Verilog frontend for an usage example.
 *
 */

#include "kernel/log.h"
#include "libs/sha1/sha1.h"
#include "ast.h"

#include <sstream>
#include <stdarg.h>
#include <math.h>

using namespace AST;
using namespace AST_INTERNAL;

// convert the AST into a simpler AST that has all parameters subsitited by their
// values, unrolled for-loops, expanded generate blocks, etc. when this function
// is done with an AST it can be converted into RTLIL using genRTLIL().
//
// this function also does all name resolving and sets the id2ast member of all
// nodes that link to a different node using names and lexical scoping.
bool AstNode::simplify(bool const_fold, bool at_zero, bool in_lvalue, int stage, int width_hint, bool sign_hint, bool in_param)
{
	AstNode *newNode = NULL;
	bool did_something = false;

#if 0
	log("-------------\n");
	log("const_fold=%d, at_zero=%d, in_lvalue=%d, stage=%d, width_hint=%d, sign_hint=%d, in_param=%d\n",
			int(const_fold), int(at_zero), int(in_lvalue), int(stage), int(width_hint), int(sign_hint), int(in_param));
	dumpAst(NULL, "> ");
#endif

	if (stage == 0)
	{
		assert(type == AST_MODULE);

		while (simplify(const_fold, at_zero, in_lvalue, 1, width_hint, sign_hint, in_param)) { }

		if (!flag_nomem2reg && !get_bool_attribute("\\nomem2reg"))
		{
			std::map<AstNode*, std::set<std::string>> mem2reg_places;
			std::map<AstNode*, uint32_t> mem2reg_candidates, dummy_proc_flags;
			uint32_t flags = flag_mem2reg ? AstNode::MEM2REG_FL_ALL : 0;
			mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, dummy_proc_flags, flags);

			std::set<AstNode*> mem2reg_set;
			for (auto &it : mem2reg_candidates)
			{
				AstNode *mem = it.first;
				uint32_t memflags = it.second;
				assert((memflags & ~0x00ffff00) == 0);

				if (mem->get_bool_attribute("\\nomem2reg"))
					continue;

				if (memflags & AstNode::MEM2REG_FL_FORCED)
					goto silent_activate;

				if (memflags & AstNode::MEM2REG_FL_EQ2)
					goto verbose_activate;

				if (memflags & AstNode::MEM2REG_FL_SET_ASYNC)
					goto verbose_activate;

				if ((memflags & AstNode::MEM2REG_FL_SET_INIT) && (memflags & AstNode::MEM2REG_FL_SET_ELSE))
					goto verbose_activate;

				if (memflags & AstNode::MEM2REG_FL_CMPLX_LHS)
					goto verbose_activate;

				// log("Note: Not replacing memory %s with list of registers (flags=0x%08lx).\n", mem->str.c_str(), long(memflags));
				continue;

			verbose_activate:
				if (mem2reg_set.count(mem) == 0) {
					log("Warning: Replacing memory %s with list of registers.", mem->str.c_str());
					bool first_element = true;
					for (auto &place : mem2reg_places[it.first]) {
						log("%s%s", first_element ? " See " : ", ", place.c_str());
						first_element = false;
					}
					log("\n");
				}

			silent_activate:
				// log("Note: Replacing memory %s with list of registers (flags=0x%08lx).\n", mem->str.c_str(), long(memflags));
				mem2reg_set.insert(mem);
			}

			for (auto node : mem2reg_set)
			{
				int mem_width, mem_size, addr_bits;
				node->meminfo(mem_width, mem_size, addr_bits);

				for (int i = 0; i < mem_size; i++) {
					AstNode *reg = new AstNode(AST_WIRE, new AstNode(AST_RANGE,
							mkconst_int(mem_width-1, true), mkconst_int(0, true)));
					reg->str = stringf("%s[%d]", node->str.c_str(), i);
					reg->is_reg = true;
					reg->is_signed = node->is_signed;
					children.push_back(reg);
					while (reg->simplify(true, false, false, 1, -1, false, false)) { }
				}
			}

			mem2reg_as_needed_pass2(mem2reg_set, this, NULL);

			for (size_t i = 0; i < children.size(); i++) {
				if (mem2reg_set.count(children[i]) > 0) {
					delete children[i];
					children.erase(children.begin() + (i--));
				}
			}
		}

		while (simplify(const_fold, at_zero, in_lvalue, 2, width_hint, sign_hint, in_param)) { }
		return false;
	}

	current_filename = filename;
	set_line_num(linenum);

	// we do not look inside a task or function
	// (but as soon as a task of function is instanciated we process the generated AST as usual)
	if (type == AST_FUNCTION || type == AST_TASK)
		return false;

	// deactivate all calls to non-synthesis system taks
	if ((type == AST_FCALL || type == AST_TCALL) && (str == "$display" || str == "$stop" || str == "$finish")) {
		delete_children();
		str = std::string();
	}

	// activate const folding if this is anything that must be evaluated statically (ranges, parameters, attributes, etc.)
	if (type == AST_WIRE || type == AST_PARAMETER || type == AST_LOCALPARAM || type == AST_DEFPARAM || type == AST_PARASET || type == AST_RANGE || type == AST_PREFIX)
		const_fold = true;
	if (type == AST_IDENTIFIER && current_scope.count(str) > 0 && (current_scope[str]->type == AST_PARAMETER || current_scope[str]->type == AST_LOCALPARAM))
		const_fold = true;

	// in certain cases a function must be evaluated constant. this is what in_param controls.
	if (type == AST_PARAMETER || type == AST_LOCALPARAM || type == AST_DEFPARAM || type == AST_PARASET || type == AST_PREFIX)
		in_param = true;

	std::map<std::string, AstNode*> backup_scope;

	// create name resolution entries for all objects with names
	// also merge multiple declarations for the same wire (e.g. "output foobar; reg foobar;")
	if (type == AST_MODULE) {
		current_scope.clear();
		std::map<std::string, AstNode*> this_wire_scope;
		for (size_t i = 0; i < children.size(); i++) {
			AstNode *node = children[i];
			if (node->type == AST_WIRE) {
				if (this_wire_scope.count(node->str) > 0) {
					AstNode *first_node = this_wire_scope[node->str];
					if (!node->is_input && !node->is_output && node->is_reg && node->children.size() == 0)
						goto wires_are_compatible;
					if (first_node->children.size() != node->children.size())
						goto wires_are_incompatible;
					for (size_t j = 0; j < node->children.size(); j++) {
						AstNode *n1 = first_node->children[j], *n2 = node->children[j];
						if (n1->type == AST_RANGE && n2->type == AST_RANGE && n1->range_valid && n2->range_valid) {
							if (n1->range_left != n2->range_left)
								goto wires_are_incompatible;
							if (n1->range_right != n2->range_right)
								goto wires_are_incompatible;
						} else if (*n1 != *n2)
							goto wires_are_incompatible;
					}
					if (first_node->range_left != node->range_left)
						goto wires_are_incompatible;
					if (first_node->range_right != node->range_right)
						goto wires_are_incompatible;
					if (first_node->port_id == 0 && (node->is_input || node->is_output))
						goto wires_are_incompatible;
				wires_are_compatible:
					if (node->is_input)
						first_node->is_input = true;
					if (node->is_output)
						first_node->is_output = true;
					if (node->is_reg)
						first_node->is_reg = true;
					if (node->is_signed)
						first_node->is_signed = true;
					for (auto &it : node->attributes) {
						if (first_node->attributes.count(it.first) > 0)
							delete first_node->attributes[it.first];
						first_node->attributes[it.first] = it.second->clone();
					}
					children.erase(children.begin()+(i--));
					did_something = true;
					delete node;
					continue;
				wires_are_incompatible:
					if (stage > 1)
						log_error("Incompatible re-declaration of wire %s at %s:%d.\n", node->str.c_str(), filename.c_str(), linenum);
					continue;
				}
				this_wire_scope[node->str] = node;
			}
			if (node->type == AST_PARAMETER || node->type == AST_LOCALPARAM || node->type == AST_WIRE || node->type == AST_AUTOWIRE || node->type == AST_GENVAR ||
					node->type == AST_MEMORY || node->type == AST_FUNCTION || node->type == AST_TASK || node->type == AST_CELL) {
				backup_scope[node->str] = current_scope[node->str];
				current_scope[node->str] = node;
			}
		}
		for (size_t i = 0; i < children.size(); i++) {
			AstNode *node = children[i];
			if (node->type == AST_PARAMETER || node->type == AST_LOCALPARAM || node->type == AST_WIRE || node->type == AST_AUTOWIRE)
				while (node->simplify(true, false, false, 1, -1, false, node->type == AST_PARAMETER || node->type == AST_LOCALPARAM))
					did_something = true;
		}
	}

	auto backup_current_block = current_block;
	auto backup_current_block_child = current_block_child;
	auto backup_current_top_block = current_top_block;

	int backup_width_hint = width_hint;
	bool backup_sign_hint = sign_hint;

	bool detect_width_simple = false;
	bool child_0_is_self_determined = false;
	bool child_1_is_self_determined = false;
	bool child_2_is_self_determined = false;
	bool children_are_self_determined = false;
	bool reset_width_after_children = false;

	switch (type)
	{
	case AST_ASSIGN_EQ:
	case AST_ASSIGN_LE:
	case AST_ASSIGN:
		while (!children[0]->basic_prep && children[0]->simplify(false, false, true, stage, -1, false, in_param) == true)
			did_something = true;
		while (!children[1]->basic_prep && children[1]->simplify(false, false, false, stage, -1, false, in_param) == true)
			did_something = true;
		children[0]->detectSignWidth(backup_width_hint, backup_sign_hint);
		children[1]->detectSignWidth(width_hint, sign_hint);
		width_hint = std::max(width_hint, backup_width_hint);
		child_0_is_self_determined = true;
		break;

	case AST_PARAMETER:
	case AST_LOCALPARAM:
		while (!children[0]->basic_prep && children[0]->simplify(false, false, false, stage, -1, false, true) == true)
			did_something = true;
		children[0]->detectSignWidth(width_hint, sign_hint);
		if (children.size() > 1 && children[1]->type == AST_RANGE) {
			while (!children[1]->basic_prep && children[1]->simplify(false, false, false, stage, -1, false, true) == true)
				did_something = true;
			if (!children[1]->range_valid)
				log_error("Non-constant width range on parameter decl at %s:%d.\n", filename.c_str(), linenum);
			width_hint = std::max(width_hint, children[1]->range_left - children[1]->range_right + 1);
		}
		break;

	case AST_TO_BITS:
	case AST_TO_SIGNED:
	case AST_TO_UNSIGNED:
	case AST_CONCAT:
	case AST_REPLICATE:
	case AST_REDUCE_AND:
	case AST_REDUCE_OR:
	case AST_REDUCE_XOR:
	case AST_REDUCE_XNOR:
	case AST_REDUCE_BOOL:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	case AST_NEG:
	case AST_BIT_NOT:
	case AST_POS:
	case AST_BIT_AND:
	case AST_BIT_OR:
	case AST_BIT_XOR:
	case AST_BIT_XNOR:
	case AST_ADD:
	case AST_SUB:
	case AST_MUL:
	case AST_DIV:
	case AST_MOD:
		detect_width_simple = true;
		break;

	case AST_SHIFT_LEFT:
	case AST_SHIFT_RIGHT:
	case AST_SHIFT_SLEFT:
	case AST_SHIFT_SRIGHT:
	case AST_POW:
		detect_width_simple = true;
		child_1_is_self_determined = true;
		break;

	case AST_LT:
	case AST_LE:
	case AST_EQ:
	case AST_NE:
	case AST_EQX:
	case AST_NEX:
	case AST_GE:
	case AST_GT:
		width_hint = -1;
		sign_hint = true;
		for (auto child : children) {
			while (!child->basic_prep && child->simplify(false, false, in_lvalue, stage, -1, false, in_param) == true)
				did_something = true;
			child->detectSignWidthWorker(width_hint, sign_hint);
		}
		reset_width_after_children = true;
		break;

	case AST_LOGIC_AND:
	case AST_LOGIC_OR:
	case AST_LOGIC_NOT:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	case AST_TERNARY:
		detect_width_simple = true;
		child_0_is_self_determined = true;
		break;
	
	case AST_MEMRD:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	default:
		width_hint = -1;
		sign_hint = false;
	}

	if (detect_width_simple && width_hint < 0) {
		if (type == AST_REPLICATE)
			while (children[0]->simplify(true, false, in_lvalue, stage, -1, false, true) == true)
				did_something = true;
		for (auto child : children)
			while (!child->basic_prep && child->simplify(false, false, in_lvalue, stage, -1, false, in_param) == true)
				did_something = true;
		detectSignWidth(width_hint, sign_hint);
	}

	if (type == AST_TERNARY) {
		int width_hint_left, width_hint_right;
		bool sign_hint_left, sign_hint_right;
		bool found_real_left, found_real_right;
		children[1]->detectSignWidth(width_hint_left, sign_hint_left, &found_real_left);
		children[2]->detectSignWidth(width_hint_right, sign_hint_right, &found_real_right);
		if (found_real_left || found_real_right) {
			child_1_is_self_determined = true;
			child_2_is_self_determined = true;
		}
	}

	// simplify all children first
	// (iterate by index as e.g. auto wires can add new children in the process)
	for (size_t i = 0; i < children.size(); i++) {
		bool did_something_here = true;
		if ((type == AST_GENFOR || type == AST_FOR) && i >= 3)
			break;
		if ((type == AST_GENIF || type == AST_GENCASE) && i >= 1)
			break;
		if (type == AST_GENBLOCK)
			break;
		if (type == AST_BLOCK && !str.empty())
			break;
		if (type == AST_PREFIX && i >= 1)
			break;
		while (did_something_here && i < children.size()) {
			bool const_fold_here = const_fold, in_lvalue_here = in_lvalue;
			int width_hint_here = width_hint;
			bool sign_hint_here = sign_hint;
			bool in_param_here = in_param;
			if (i == 0 && (type == AST_REPLICATE || type == AST_WIRE))
				const_fold_here = true, in_param_here = true;
			if (type == AST_PARAMETER || type == AST_LOCALPARAM)
				const_fold_here = true;
			if (i == 0 && (type == AST_ASSIGN || type == AST_ASSIGN_EQ || type == AST_ASSIGN_LE))
				in_lvalue_here = true;
			if (type == AST_BLOCK) {
				current_block = this;
				current_block_child = children[i];
			}
			if ((type == AST_ALWAYS || type == AST_INITIAL) && children[i]->type == AST_BLOCK)
				current_top_block = children[i];
			if (i == 0 && child_0_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (i == 1 && child_1_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (i == 2 && child_2_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (children_are_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			did_something_here = children[i]->simplify(const_fold_here, at_zero, in_lvalue_here, stage, width_hint_here, sign_hint_here, in_param_here);
			if (did_something_here)
				did_something = true;
		}
		if (stage == 2 && children[i]->type == AST_INITIAL && current_ast_mod != this) {
			current_ast_mod->children.push_back(children[i]);
			children.erase(children.begin() + (i--));
			did_something = true;
		}
	}
	for (auto &attr : attributes) {
		while (attr.second->simplify(true, false, false, stage, -1, false, true))
			did_something = true;
	}

	if (reset_width_after_children) {
		width_hint = backup_width_hint;
		sign_hint = backup_sign_hint;
		if (width_hint < 0)
			detectSignWidth(width_hint, sign_hint);
	}

	current_block = backup_current_block;
	current_block_child = backup_current_block_child;
	current_top_block = backup_current_top_block;

	for (auto it = backup_scope.begin(); it != backup_scope.end(); it++) {
		if (it->second == NULL)
			current_scope.erase(it->first);
		else
			current_scope[it->first] = it->second;
	}

	current_filename = filename;
	set_line_num(linenum);

	if (type == AST_MODULE)
		current_scope.clear();

	// convert defparam nodes to cell parameters
	if (type == AST_DEFPARAM && !str.empty()) {
		size_t pos = str.rfind('.');
		if (pos == std::string::npos)
			log_error("Defparam `%s' does not contain a dot (module/parameter seperator) at %s:%d!\n",
					RTLIL::id2cstr(str.c_str()), filename.c_str(), linenum);
		std::string modname = str.substr(0, pos), paraname = "\\" + str.substr(pos+1);
		if (current_scope.count(modname) == 0 || current_scope.at(modname)->type != AST_CELL)
			log_error("Can't find cell for defparam `%s . %s` at %s:%d!\n", RTLIL::id2cstr(modname), RTLIL::id2cstr(paraname), filename.c_str(), linenum);
		AstNode *cell = current_scope.at(modname), *paraset = clone();
		cell->children.insert(cell->children.begin() + 1, paraset);
		paraset->type = AST_PARASET;
		paraset->str = paraname;
		str.clear();
	}

	// resolve constant prefixes
	if (type == AST_PREFIX) {
		if (children[0]->type != AST_CONSTANT) {
			// dumpAst(NULL, ">   ");
			log_error("Index in generate block prefix syntax at %s:%d is not constant!\n", filename.c_str(), linenum);
		}
		assert(children[1]->type == AST_IDENTIFIER);
		newNode = children[1]->clone();
		const char *second_part = children[1]->str.c_str();
		if (second_part[0] == '\\')
			second_part++;
		newNode->str = stringf("%s[%d].%s", str.c_str(), children[0]->integer, second_part);
		goto apply_newNode;
	}

	// evaluate TO_BITS nodes
	if (type == AST_TO_BITS) {
		if (children[0]->type != AST_CONSTANT)
			log_error("Left operand of to_bits expression is not constant at %s:%d!\n", filename.c_str(), linenum);
		if (children[1]->type != AST_CONSTANT)
			log_error("Right operand of to_bits expression is not constant at %s:%d!\n", filename.c_str(), linenum);
		RTLIL::Const new_value = children[1]->bitsAsConst(children[0]->bitsAsConst().as_int(), children[1]->is_signed);
		newNode = mkconst_bits(new_value.bits, children[1]->is_signed);
		goto apply_newNode;
	}

	// annotate constant ranges
	if (type == AST_RANGE) {
		bool old_range_valid = range_valid;
		range_valid = false;
		range_left = -1;
		range_right = 0;
		assert(children.size() >= 1);
		if (children[0]->type == AST_CONSTANT) {
			range_valid = true;
			range_left = children[0]->integer;
			if (children.size() == 1)
				range_right = range_left;
		}
		if (children.size() >= 2) {
			if (children[1]->type == AST_CONSTANT)
				range_right = children[1]->integer;
			else
				range_valid = false;
		}
		if (old_range_valid != range_valid)
			did_something = true;
		if (range_valid && range_left >= 0 && range_right > range_left) {
			int tmp = range_right;
			range_right = range_left;
			range_left = tmp;
		}
	}

	// annotate wires with their ranges
	if (type == AST_WIRE) {
		if (children.size() > 0) {
			if (children[0]->range_valid) {
				if (!range_valid)
					did_something = true;
				range_valid = true;
				range_left = children[0]->range_left;
				range_right = children[0]->range_right;
			}
		} else {
			if (!range_valid)
				did_something = true;
			range_valid = true;
			range_left = 0;
			range_right = 0;
		}
	}

	// trim/extend parameters
	if (type == AST_PARAMETER || type == AST_LOCALPARAM) {
		if (children.size() > 1 && children[1]->type == AST_RANGE) {
			if (!children[1]->range_valid)
				log_error("Non-constant width range on parameter decl at %s:%d.\n", filename.c_str(), linenum);
			int width = children[1]->range_left - children[1]->range_right + 1;
			if (children[0]->type == AST_REALVALUE) {
				RTLIL::Const constvalue = children[0]->realAsConst(width);
				log("Warning: converting real value %e to binary %s at %s:%d.\n",
						children[0]->realvalue, log_signal(constvalue), filename.c_str(), linenum);
				delete children[0];
				children[0] = mkconst_bits(constvalue.bits, sign_hint);
				did_something = true;
			}
			if (children[0]->type == AST_CONSTANT) {
				if (width != int(children[0]->bits.size())) {
					RTLIL::SigSpec sig(children[0]->bits);
					sig.extend_u0(width, children[0]->is_signed);
					AstNode *old_child_0 = children[0];
					children[0] = mkconst_bits(sig.as_const().bits, children[0]->is_signed);
					delete old_child_0;
				}
				children[0]->is_signed = is_signed;
			}
		} else
		if (children.size() > 1 && children[1]->type == AST_REALVALUE && children[0]->type == AST_CONSTANT) {
			double as_realvalue = children[0]->asReal(sign_hint);
			delete children[0];
			children[0] = new AstNode(AST_REALVALUE);
			children[0]->realvalue = as_realvalue;
			did_something = true;
		}
	}

	// annotate identifiers using scope resolution and create auto-wires as needed
	if (type == AST_IDENTIFIER) {
		if (current_scope.count(str) == 0) {
			for (auto node : current_ast_mod->children) {
				if ((node->type == AST_PARAMETER || node->type == AST_LOCALPARAM || node->type == AST_WIRE || node->type == AST_AUTOWIRE || node->type == AST_GENVAR ||
						node->type == AST_MEMORY || node->type == AST_FUNCTION || node->type == AST_TASK) && str == node->str) {
					current_scope[node->str] = node;
					break;
				}
			}
		}
		if (current_scope.count(str) == 0) {
			// log("Warning: Creating auto-wire `%s' in module `%s'.\n", str.c_str(), current_ast_mod->str.c_str());
			AstNode *auto_wire = new AstNode(AST_AUTOWIRE);
			auto_wire->str = str;
			current_ast_mod->children.push_back(auto_wire);
			current_scope[str] = auto_wire;
			did_something = true;
		}
		if (id2ast != current_scope[str]) {
			id2ast = current_scope[str];
			did_something = true;
		}
	}

	// split memory access with bit select to individual statements
	if (type == AST_IDENTIFIER && children.size() == 2 && children[0]->type == AST_RANGE && children[1]->type == AST_RANGE)
	{
		if (id2ast == NULL || id2ast->type != AST_MEMORY || children[0]->children.size() != 1 || in_lvalue)
			log_error("Invalid bit-select on memory access at %s:%d!\n", filename.c_str(), linenum);

		int mem_width, mem_size, addr_bits;
		id2ast->meminfo(mem_width, mem_size, addr_bits);

		std::stringstream sstr;
		sstr << "$mem2bits$" << children[0]->str << "$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
		std::string wire_id = sstr.str();

		AstNode *wire = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(mem_width-1, true), mkconst_int(0, true)));
		wire->str = wire_id;
		if (current_block)
			wire->attributes["\\nosync"] = AstNode::mkconst_int(1, false);
		current_ast_mod->children.push_back(wire);
		while (wire->simplify(true, false, false, 1, -1, false, false)) { }

		AstNode *data = clone();
		delete data->children[1];
		data->children.pop_back();

		AstNode *assign = new AstNode(AST_ASSIGN_EQ, new AstNode(AST_IDENTIFIER), data);
		assign->children[0]->str = wire_id;

		if (current_block)
		{
			size_t assign_idx = 0;
			while (assign_idx < current_block->children.size() && current_block->children[assign_idx] != current_block_child)
				assign_idx++;
			log_assert(assign_idx < current_block->children.size());
			current_block->children.insert(current_block->children.begin()+assign_idx, assign);
			wire->is_reg = true;
		}
		else
		{
			AstNode *proc = new AstNode(AST_ALWAYS, new AstNode(AST_BLOCK));
			proc->children[0]->children.push_back(assign);
			current_ast_mod->children.push_back(proc);
		}

		newNode = new AstNode(AST_IDENTIFIER, children[1]->clone());
		newNode->str = wire_id;
		newNode->id2ast = wire;
		goto apply_newNode;
	}

	if (type == AST_WHILE)
		log_error("While loops are only allowed in constant functions at %s:%d!\n", filename.c_str(), linenum);

	if (type == AST_REPEAT)
		log_error("Repeat loops are only allowed in constant functions at %s:%d!\n", filename.c_str(), linenum);

	// unroll for loops and generate-for blocks
	if ((type == AST_GENFOR || type == AST_FOR) && children.size() != 0)
	{
		AstNode *init_ast = children[0];
		AstNode *while_ast = children[1];
		AstNode *next_ast = children[2];
		AstNode *body_ast = children[3];

		while (body_ast->type == AST_GENBLOCK && body_ast->str.empty() &&
				body_ast->children.size() == 1 && body_ast->children.at(0)->type == AST_GENBLOCK)
			body_ast = body_ast->children.at(0);

		if (init_ast->type != AST_ASSIGN_EQ)
			log_error("Unsupported 1st expression of generate for-loop at %s:%d!\n", filename.c_str(), linenum);
		if (next_ast->type != AST_ASSIGN_EQ)
			log_error("Unsupported 3rd expression of generate for-loop at %s:%d!\n", filename.c_str(), linenum);

		if (type == AST_GENFOR) {
			if (init_ast->children[0]->id2ast == NULL || init_ast->children[0]->id2ast->type != AST_GENVAR)
				log_error("Left hand side of 1st expression of generate for-loop at %s:%d is not a gen var!\n", filename.c_str(), linenum);
			if (next_ast->children[0]->id2ast == NULL || next_ast->children[0]->id2ast->type != AST_GENVAR)
				log_error("Left hand side of 3rd expression of generate for-loop at %s:%d is not a gen var!\n", filename.c_str(), linenum);
		} else {
			if (init_ast->children[0]->id2ast == NULL || init_ast->children[0]->id2ast->type != AST_WIRE)
				log_error("Left hand side of 1st expression of generate for-loop at %s:%d is not a register!\n", filename.c_str(), linenum);
			if (next_ast->children[0]->id2ast == NULL || next_ast->children[0]->id2ast->type != AST_WIRE)
				log_error("Left hand side of 3rd expression of generate for-loop at %s:%d is not a register!\n", filename.c_str(), linenum);
		}

		if (init_ast->children[0]->id2ast != next_ast->children[0]->id2ast)
			log_error("Incompatible left-hand sides in 1st and 3rd expression of generate for-loop at %s:%d!\n", filename.c_str(), linenum);

		// eval 1st expression
		AstNode *varbuf = init_ast->children[1]->clone();
		while (varbuf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }

		if (varbuf->type != AST_CONSTANT)
			log_error("Right hand side of 1st expression of generate for-loop at %s:%d is not constant!\n", filename.c_str(), linenum);

		varbuf = new AstNode(AST_LOCALPARAM, varbuf);
		varbuf->str = init_ast->children[0]->str;

		AstNode *backup_scope_varbuf = current_scope[varbuf->str];
		current_scope[varbuf->str] = varbuf;

		size_t current_block_idx = 0;
		if (type == AST_FOR) {
			while (current_block_idx < current_block->children.size() &&
					current_block->children[current_block_idx] != current_block_child)
				current_block_idx++;
		}

		while (1)
		{
			// eval 2nd expression
			AstNode *buf = while_ast->clone();
			while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }

			if (buf->type != AST_CONSTANT)
				log_error("2nd expression of generate for-loop at %s:%d is not constant!\n", filename.c_str(), linenum);

			if (buf->integer == 0) {
				delete buf;
				break;
			}
			delete buf;

			// expand body
			int index = varbuf->children[0]->integer;
			if (body_ast->type == AST_GENBLOCK)
				buf = body_ast->clone();
			else
				buf = new AstNode(AST_GENBLOCK, body_ast->clone());
			if (buf->str.empty()) {
				std::stringstream sstr;
				sstr << "$genblock$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
				buf->str = sstr.str();
			}
			std::map<std::string, std::string> name_map;
			std::stringstream sstr;
			sstr << buf->str << "[" << index << "].";
			buf->expand_genblock(varbuf->str, sstr.str(), name_map);

			if (type == AST_GENFOR) {
				for (size_t i = 0; i < buf->children.size(); i++) {
					buf->children[i]->simplify(false, false, false, stage, -1, false, false);
					current_ast_mod->children.push_back(buf->children[i]);
				}
			} else {
				for (size_t i = 0; i < buf->children.size(); i++)
					current_block->children.insert(current_block->children.begin() + current_block_idx++, buf->children[i]);
			}
			buf->children.clear();
			delete buf;

			// eval 3rd expression
			buf = next_ast->children[1]->clone();
			while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }

			if (buf->type != AST_CONSTANT)
				log_error("Right hand side of 3rd expression of generate for-loop at %s:%d is not constant!\n", filename.c_str(), linenum);

			delete varbuf->children[0];
			varbuf->children[0] = buf;
		}

		current_scope[varbuf->str] = backup_scope_varbuf;
		delete varbuf;
		delete_children();
		did_something = true;
	}

	// transform block with name
	if (type == AST_BLOCK && !str.empty())
	{
		std::map<std::string, std::string> name_map;
		expand_genblock(std::string(), str + ".", name_map);

		std::vector<AstNode*> new_children;
		for (size_t i = 0; i < children.size(); i++)
			if (children[i]->type == AST_WIRE) {
				children[i]->simplify(false, false, false, stage, -1, false, false);
				current_ast_mod->children.push_back(children[i]);
			} else
				new_children.push_back(children[i]);

		children.swap(new_children);
		did_something = true;
		str.clear();
	}

	// simplify unconditional generate block
	if (type == AST_GENBLOCK && children.size() != 0)
	{
		if (!str.empty()) {
			std::map<std::string, std::string> name_map;
			expand_genblock(std::string(), str + ".", name_map);
		}

		for (size_t i = 0; i < children.size(); i++) {
			children[i]->simplify(false, false, false, stage, -1, false, false);
			current_ast_mod->children.push_back(children[i]);
		}

		children.clear();
		did_something = true;
	}

	// simplify generate-if blocks
	if (type == AST_GENIF && children.size() != 0)
	{
		AstNode *buf = children[0]->clone();
		while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
		if (buf->type != AST_CONSTANT) {
			// for (auto f : log_files)
			// 	dumpAst(f, "verilog-ast> ");
			log_error("Condition for generate if at %s:%d is not constant!\n", filename.c_str(), linenum);
		}
		if (buf->asBool() != 0) {
			delete buf;
			buf = children[1]->clone();
		} else {
			delete buf;
			buf = children.size() > 2 ? children[2]->clone() : NULL;
		}

		if (buf)
		{
			if (buf->type != AST_GENBLOCK)
				buf = new AstNode(AST_GENBLOCK, buf);

			if (!buf->str.empty()) {
				std::map<std::string, std::string> name_map;
				buf->expand_genblock(std::string(), buf->str + ".", name_map);
			}

			for (size_t i = 0; i < buf->children.size(); i++) {
				buf->children[i]->simplify(false, false, false, stage, -1, false, false);
				current_ast_mod->children.push_back(buf->children[i]);
			}

			buf->children.clear();
			delete buf;
		}

		delete_children();
		did_something = true;
	}

	// simplify generate-case blocks
	if (type == AST_GENCASE && children.size() != 0)
	{
		AstNode *buf = children[0]->clone();
		while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
		if (buf->type != AST_CONSTANT) {
			// for (auto f : log_files)
			// 	dumpAst(f, "verilog-ast> ");
			log_error("Condition for generate case at %s:%d is not constant!\n", filename.c_str(), linenum);
		}

		bool ref_signed = buf->is_signed;
		RTLIL::Const ref_value = buf->bitsAsConst();
		delete buf;

		AstNode *selected_case = NULL;
		for (size_t i = 1; i < children.size(); i++)
		{
			log_assert(children.at(i)->type == AST_COND);

			AstNode *this_genblock = NULL;
			for (auto child : children.at(i)->children) {
				log_assert(this_genblock == NULL);
				if (child->type == AST_GENBLOCK)
					this_genblock = child;
			}

			for (auto child : children.at(i)->children)
			{
				if (child->type == AST_DEFAULT) {
					if (selected_case == NULL)
						selected_case = this_genblock;
					continue;
				}
				if (child->type == AST_GENBLOCK)
					continue;

				buf = child->clone();
				while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
				if (buf->type != AST_CONSTANT) {
					// for (auto f : log_files)
					// 	dumpAst(f, "verilog-ast> ");
					log_error("Expression in generate case at %s:%d is not constant!\n", filename.c_str(), linenum);
				}

				if (RTLIL::const_eq(ref_value, buf->bitsAsConst(), ref_signed && buf->is_signed, ref_signed && buf->is_signed, 1).as_bool()) {
					selected_case = this_genblock;
					i = children.size();
					break;
				}
			}
		}

		if (selected_case != NULL)
		{
			log_assert(selected_case->type == AST_GENBLOCK);
			buf = selected_case->clone();

			if (!buf->str.empty()) {
				std::map<std::string, std::string> name_map;
				buf->expand_genblock(std::string(), buf->str + ".", name_map);
			}

			for (size_t i = 0; i < buf->children.size(); i++) {
				buf->children[i]->simplify(false, false, false, stage, -1, false, false);
				current_ast_mod->children.push_back(buf->children[i]);
			}

			buf->children.clear();
			delete buf;
		}

		delete_children();
		did_something = true;
	}

	// unroll cell arrays
	if (type == AST_CELLARRAY)
	{
		if (!children.at(0)->range_valid)
			log_error("Non-constant array range on cell array at %s:%d.\n", filename.c_str(), linenum);

		newNode = new AstNode(AST_GENBLOCK);
		int num = std::max(children.at(0)->range_left, children.at(0)->range_right) - std::min(children.at(0)->range_left, children.at(0)->range_right) + 1;

		for (int i = 0; i < num; i++) {
			int idx = children.at(0)->range_left > children.at(0)->range_right ? children.at(0)->range_right + i : children.at(0)->range_right - i;
			AstNode *new_cell = children.at(1)->clone();
			newNode->children.push_back(new_cell);
			new_cell->str += stringf("[%d]", idx);
			if (new_cell->type == AST_PRIMITIVE) {
				log_error("Cell arrays of primitives are currently not supported at %s:%d.\n", filename.c_str(), linenum);
			} else {
				log_assert(new_cell->children.at(0)->type == AST_CELLTYPE);
				new_cell->children.at(0)->str = stringf("$array:%d:%d:%s", i, num, new_cell->children.at(0)->str.c_str());
			}
		}

		goto apply_newNode;
	}

	// replace primitives with assignmens
	if (type == AST_PRIMITIVE)
	{
		if (children.size() < 2)
			log_error("Insufficient number of arguments for primitive `%s' at %s:%d!\n",
					str.c_str(), filename.c_str(), linenum);

		std::vector<AstNode*> children_list;
		for (auto child : children) {
			assert(child->type == AST_ARGUMENT);
			assert(child->children.size() == 1);
			children_list.push_back(child->children[0]);
			child->children.clear();
			delete child;
		}
		children.clear();

		if (str == "bufif0" || str == "bufif1" || str == "notif0" || str == "notif1")
		{
			if (children_list.size() != 3)
				log_error("Invalid number of arguments for primitive `%s' at %s:%d!\n",
						str.c_str(), filename.c_str(), linenum);

			std::vector<RTLIL::State> z_const(1, RTLIL::State::Sz);

			AstNode *mux_input = children_list.at(1);
			if (str == "notif0" || str == "notif1") {
				mux_input = new AstNode(AST_BIT_NOT, mux_input);
			}
			AstNode *node = new AstNode(AST_TERNARY, children_list.at(2));
			if (str == "bufif0") {
				node->children.push_back(AstNode::mkconst_bits(z_const, false));
				node->children.push_back(mux_input);
			} else {
				node->children.push_back(mux_input);
				node->children.push_back(AstNode::mkconst_bits(z_const, false));
			}

			str.clear();
			type = AST_ASSIGN;
			children.push_back(children_list.at(0));
			children.push_back(node);
			did_something = true;
		}
		else
		{
			AstNodeType op_type = AST_NONE;
			bool invert_results = false;

			if (str == "and")
				op_type = AST_BIT_AND;
			if (str == "nand")
				op_type = AST_BIT_AND, invert_results = true;
			if (str == "or")
				op_type = AST_BIT_OR;
			if (str == "nor")
				op_type = AST_BIT_OR, invert_results = true;
			if (str == "xor")
				op_type = AST_BIT_XOR;
			if (str == "xnor")
				op_type = AST_BIT_XOR, invert_results = true;
			if (str == "buf")
				op_type = AST_POS;
			if (str == "not")
				op_type = AST_POS, invert_results = true;
			assert(op_type != AST_NONE);

			AstNode *node = children_list[1];
			if (op_type != AST_POS)
				for (size_t i = 2; i < children_list.size(); i++)
					node = new AstNode(op_type, node, children_list[i]);
			if (invert_results)
				node = new AstNode(AST_BIT_NOT, node);

			str.clear();
			type = AST_ASSIGN;
			children.push_back(children_list[0]);
			children.push_back(node);
			did_something = true;
		}
	}

	// replace dynamic ranges in left-hand side expressions (e.g. "foo[bar] <= 1'b1;") with
	// a big case block that selects the correct single-bit assignment.
	if (type == AST_ASSIGN_EQ || type == AST_ASSIGN_LE) {
		if (children[0]->type != AST_IDENTIFIER || children[0]->children.size() == 0)
			goto skip_dynamic_range_lvalue_expansion;
		if (children[0]->children[0]->range_valid || did_something)
			goto skip_dynamic_range_lvalue_expansion;
		if (children[0]->id2ast == NULL || children[0]->id2ast->type != AST_WIRE)
			goto skip_dynamic_range_lvalue_expansion;
		if (!children[0]->id2ast->range_valid)
			goto skip_dynamic_range_lvalue_expansion;
		int source_width = children[0]->id2ast->range_left - children[0]->id2ast->range_right + 1;
		int result_width = 1;
		AstNode *shift_expr = NULL;
		AstNode *range = children[0]->children[0];
		if (range->children.size() == 1) {
			shift_expr = range->children[0]->clone();
		} else {
			shift_expr = range->children[1]->clone();
			AstNode *left_at_zero_ast = range->children[0]->clone();
			AstNode *right_at_zero_ast = range->children[1]->clone();
			while (left_at_zero_ast->simplify(true, true, false, stage, -1, false, false)) { }
			while (right_at_zero_ast->simplify(true, true, false, stage, -1, false, false)) { }
			if (left_at_zero_ast->type != AST_CONSTANT || right_at_zero_ast->type != AST_CONSTANT)
				log_error("Unsupported expression on dynamic range select on signal `%s' at %s:%d!\n",
						str.c_str(), filename.c_str(), linenum);
			result_width = left_at_zero_ast->integer - right_at_zero_ast->integer + 1;
		}
		did_something = true;
		newNode = new AstNode(AST_CASE, shift_expr);
		for (int i = 0; i <= source_width-result_width; i++) {
			int start_bit = children[0]->id2ast->range_right + i;
			AstNode *cond = new AstNode(AST_COND, mkconst_int(start_bit, true));
			AstNode *lvalue = children[0]->clone();
			lvalue->delete_children();
			lvalue->children.push_back(new AstNode(AST_RANGE,
					mkconst_int(start_bit+result_width-1, true), mkconst_int(start_bit, true)));
			cond->children.push_back(new AstNode(AST_BLOCK, new AstNode(type, lvalue, children[1]->clone())));
			newNode->children.push_back(cond);
		}
		goto apply_newNode;
	}
skip_dynamic_range_lvalue_expansion:;

	if (stage > 1 && type == AST_ASSERT && current_block != NULL)
	{
		std::stringstream sstr;
		sstr << "$assert$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
		std::string id_check = sstr.str() + "_CHECK", id_en = sstr.str() + "_EN";

		AstNode *wire_check = new AstNode(AST_WIRE);
		wire_check->str = id_check;
		current_ast_mod->children.push_back(wire_check);
		current_scope[wire_check->str] = wire_check;
		while (wire_check->simplify(true, false, false, 1, -1, false, false)) { }

		AstNode *wire_en = new AstNode(AST_WIRE);
		wire_en->str = id_en;
		current_ast_mod->children.push_back(wire_en);
		current_ast_mod->children.push_back(new AstNode(AST_INITIAL, new AstNode(AST_BLOCK, new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), AstNode::mkconst_int(0, false, 1)))));
		current_ast_mod->children.back()->children[0]->children[0]->children[0]->str = id_en;
		current_scope[wire_en->str] = wire_en;
		while (wire_en->simplify(true, false, false, 1, -1, false, false)) { }

		std::vector<RTLIL::State> x_bit;
		x_bit.push_back(RTLIL::State::Sx);

		AstNode *assign_check = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_bits(x_bit, false));
		assign_check->children[0]->str = id_check;

		AstNode *assign_en = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_int(0, false, 1));
		assign_en->children[0]->str = id_en;

		AstNode *default_signals = new AstNode(AST_BLOCK);
		default_signals->children.push_back(assign_check);
		default_signals->children.push_back(assign_en);
		current_top_block->children.insert(current_top_block->children.begin(), default_signals);

		assign_check = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), new AstNode(AST_REDUCE_BOOL, children[0]->clone()));
		assign_check->children[0]->str = id_check;

		assign_en = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_int(1, false, 1));
		assign_en->children[0]->str = id_en;

		newNode = new AstNode(AST_BLOCK);
		newNode->children.push_back(assign_check);
		newNode->children.push_back(assign_en);

		AstNode *assertnode = new AstNode(AST_ASSERT);
		assertnode->children.push_back(new AstNode(AST_IDENTIFIER));
		assertnode->children.push_back(new AstNode(AST_IDENTIFIER));
		assertnode->children[0]->str = id_check;
		assertnode->children[1]->str = id_en;
		assertnode->attributes.swap(attributes);
		current_ast_mod->children.push_back(assertnode);

		goto apply_newNode;
	}

	if (stage > 1 && type == AST_ASSERT && children.size() == 1)
	{
		children[0] = new AstNode(AST_REDUCE_BOOL, children[0]->clone());
		children.push_back(mkconst_int(1, false, 1));
		did_something = true;
	}

	// found right-hand side identifier for memory -> replace with memory read port
	if (stage > 1 && type == AST_IDENTIFIER && id2ast != NULL && id2ast->type == AST_MEMORY && !in_lvalue &&
			children[0]->type == AST_RANGE && children[0]->children.size() == 1) {
		newNode = new AstNode(AST_MEMRD, children[0]->children[0]->clone());
		newNode->str = str;
		newNode->id2ast = id2ast;
		goto apply_newNode;
	}

	// assignment with memory in left-hand side expression -> replace with memory write port
	if (stage > 1 && (type == AST_ASSIGN_EQ || type == AST_ASSIGN_LE) && children[0]->type == AST_IDENTIFIER &&
			children[0]->children.size() == 1 && children[0]->id2ast && children[0]->id2ast->type == AST_MEMORY &&
			children[0]->id2ast->children.size() >= 2 && children[0]->id2ast->children[0]->range_valid &&
			children[0]->id2ast->children[1]->range_valid)
	{
		std::stringstream sstr;
		sstr << "$memwr$" << children[0]->str << "$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
		std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA", id_en = sstr.str() + "_EN";

		if (type == AST_ASSIGN_EQ)
			log("Warining: Blocking assignment to memory in line %s:%d is handled like a non-blocking assignment.\n",
					filename.c_str(), linenum);

		int mem_width, mem_size, addr_bits;
		children[0]->id2ast->meminfo(mem_width, mem_size, addr_bits);

		AstNode *wire_addr = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(addr_bits-1, true), mkconst_int(0, true)));
		wire_addr->str = id_addr;
		current_ast_mod->children.push_back(wire_addr);
		current_scope[wire_addr->str] = wire_addr;
		while (wire_addr->simplify(true, false, false, 1, -1, false, false)) { }

		AstNode *wire_data = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(mem_width-1, true), mkconst_int(0, true)));
		wire_data->str = id_data;
		current_ast_mod->children.push_back(wire_data);
		current_scope[wire_data->str] = wire_data;
		while (wire_data->simplify(true, false, false, 1, -1, false, false)) { }

		AstNode *wire_en = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(mem_width-1, true), mkconst_int(0, true)));
		wire_en->str = id_en;
		current_ast_mod->children.push_back(wire_en);
		current_scope[wire_en->str] = wire_en;
		while (wire_en->simplify(true, false, false, 1, -1, false, false)) { }

		std::vector<RTLIL::State> x_bits_addr, x_bits_data, set_bits_en;
		for (int i = 0; i < addr_bits; i++)
			x_bits_addr.push_back(RTLIL::State::Sx);
		for (int i = 0; i < mem_width; i++)
			x_bits_data.push_back(RTLIL::State::Sx);
		for (int i = 0; i < mem_width; i++)
			set_bits_en.push_back(RTLIL::State::S1);

		AstNode *assign_addr = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_bits(x_bits_addr, false));
		assign_addr->children[0]->str = id_addr;

		AstNode *assign_data = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_bits(x_bits_data, false));
		assign_data->children[0]->str = id_data;

		AstNode *assign_en = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_int(0, false, mem_width));
		assign_en->children[0]->str = id_en;

		AstNode *default_signals = new AstNode(AST_BLOCK);
		default_signals->children.push_back(assign_addr);
		default_signals->children.push_back(assign_data);
		default_signals->children.push_back(assign_en);
		current_top_block->children.insert(current_top_block->children.begin(), default_signals);

		assign_addr = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), children[0]->children[0]->children[0]->clone());
		assign_addr->children[0]->str = id_addr;

		assign_data = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), children[1]->clone());
		assign_data->children[0]->str = id_data;

		assign_en = new AstNode(AST_ASSIGN_LE, new AstNode(AST_IDENTIFIER), mkconst_bits(set_bits_en, false));
		assign_en->children[0]->str = id_en;

		newNode = new AstNode(AST_BLOCK);
		newNode->children.push_back(assign_addr);
		newNode->children.push_back(assign_data);
		newNode->children.push_back(assign_en);

		AstNode *wrnode = new AstNode(AST_MEMWR);
		wrnode->children.push_back(new AstNode(AST_IDENTIFIER));
		wrnode->children.push_back(new AstNode(AST_IDENTIFIER));
		wrnode->children.push_back(new AstNode(AST_IDENTIFIER));
		wrnode->str = children[0]->str;
		wrnode->children[0]->str = id_addr;
		wrnode->children[1]->str = id_data;
		wrnode->children[2]->str = id_en;
		current_ast_mod->children.push_back(wrnode);

		goto apply_newNode;
	}

	// replace function and task calls with the code from the function or task
	if ((type == AST_FCALL || type == AST_TCALL) && !str.empty())
	{
		if (type == AST_FCALL)
		{
			if (str == "\\$clog2")
			{
				if (children.size() != 1)
					log_error("System function %s got %d arguments, expected 1 at %s:%d.\n",
							RTLIL::id2cstr(str), int(children.size()), filename.c_str(), linenum);

				AstNode *buf = children[0]->clone();
				while (buf->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
				if (buf->type != AST_CONSTANT)
					log_error("Failed to evaluate system function `%s' with non-constant value at %s:%d.\n", str.c_str(), filename.c_str(), linenum);

				RTLIL::Const arg_value = buf->bitsAsConst();
				uint32_t result = 0;
				for (size_t i = 0; i < arg_value.bits.size(); i++)
					if (arg_value.bits.at(i) == RTLIL::State::S1)
						result = i;

				newNode = mkconst_int(result, false);
				goto apply_newNode;
			}

			if (str == "\\$ln" || str == "\\$log10" || str == "\\$exp" || str == "\\$sqrt" || str == "\\$pow" ||
					str == "\\$floor" || str == "\\$ceil" || str == "\\$sin" || str == "\\$cos" || str == "\\$tan" ||
					str == "\\$asin" || str == "\\$acos" || str == "\\$atan" || str == "\\$atan2" || str == "\\$hypot" ||
					str == "\\$sinh" || str == "\\$cosh" || str == "\\$tanh" || str == "\\$asinh" || str == "\\$acosh" || str == "\\$atanh")
			{
				bool func_with_two_arguments = str == "\\$pow" || str == "\\$atan2" || str == "\\$hypot";
				double x = 0, y = 0;

				if (func_with_two_arguments) {
					if (children.size() != 2)
						log_error("System function %s got %d arguments, expected 2 at %s:%d.\n",
								RTLIL::id2cstr(str), int(children.size()), filename.c_str(), linenum);
				} else {
					if (children.size() != 1)
						log_error("System function %s got %d arguments, expected 1 at %s:%d.\n",
								RTLIL::id2cstr(str), int(children.size()), filename.c_str(), linenum);
				}

				if (children.size() >= 1) {
					while (children[0]->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
					if (!children[0]->isConst())
						log_error("Failed to evaluate system function `%s' with non-constant argument at %s:%d.\n",
								RTLIL::id2cstr(str), filename.c_str(), linenum);
					int child_width_hint = width_hint;
					bool child_sign_hint = sign_hint;
					children[0]->detectSignWidth(child_width_hint, child_sign_hint);
					x = children[0]->asReal(child_sign_hint);
				}

				if (children.size() >= 2) {
					while (children[1]->simplify(true, false, false, stage, width_hint, sign_hint, false)) { }
					if (!children[1]->isConst())
						log_error("Failed to evaluate system function `%s' with non-constant argument at %s:%d.\n",
								RTLIL::id2cstr(str), filename.c_str(), linenum);
					int child_width_hint = width_hint;
					bool child_sign_hint = sign_hint;
					children[1]->detectSignWidth(child_width_hint, child_sign_hint);
					y = children[1]->asReal(child_sign_hint);
				}

				newNode = new AstNode(AST_REALVALUE);
				if (str == "\\$ln")         newNode->realvalue = log(x);
				else if (str == "\\$log10") newNode->realvalue = log10(x);
				else if (str == "\\$exp")   newNode->realvalue = exp(x);
				else if (str == "\\$sqrt")  newNode->realvalue = sqrt(x);
				else if (str == "\\$pow")   newNode->realvalue = pow(x, y);
				else if (str == "\\$floor") newNode->realvalue = floor(x);
				else if (str == "\\$ceil")  newNode->realvalue = ceil(x);
				else if (str == "\\$sin")   newNode->realvalue = sin(x);
				else if (str == "\\$cos")   newNode->realvalue = cos(x);
				else if (str == "\\$tan")   newNode->realvalue = tan(x);
				else if (str == "\\$asin")  newNode->realvalue = asin(x);
				else if (str == "\\$acos")  newNode->realvalue = acos(x);
				else if (str == "\\$atan")  newNode->realvalue = atan(x);
				else if (str == "\\$atan2") newNode->realvalue = atan2(x, y);
				else if (str == "\\$hypot") newNode->realvalue = hypot(x, y);
				else if (str == "\\$sinh")  newNode->realvalue = sinh(x);
				else if (str == "\\$cosh")  newNode->realvalue = cosh(x);
				else if (str == "\\$tanh")  newNode->realvalue = tanh(x);
				else if (str == "\\$asinh") newNode->realvalue = asinh(x);
				else if (str == "\\$acosh") newNode->realvalue = acosh(x);
				else if (str == "\\$atanh") newNode->realvalue = atanh(x);
				else log_abort();
				goto apply_newNode;
			}

			if (current_scope.count(str) == 0 || current_scope[str]->type != AST_FUNCTION)
				log_error("Can't resolve function name `%s' at %s:%d.\n", str.c_str(), filename.c_str(), linenum);
		}
		if (type == AST_TCALL) {
			if (current_scope.count(str) == 0 || current_scope[str]->type != AST_TASK)
				log_error("Can't resolve task name `%s' at %s:%d.\n", str.c_str(), filename.c_str(), linenum);
		}

		bool recommend_const_eval = false;
		bool require_const_eval = in_param ? false : has_const_only_constructs(recommend_const_eval);
		if (in_param || recommend_const_eval || require_const_eval)
		{
			bool all_args_const = true;
			for (auto child : children) {
				while (child->simplify(true, false, false, 1, -1, false, true)) { }
				if (child->type != AST_CONSTANT)
					all_args_const = false;
			}

			if (all_args_const) {
				AstNode *func_workspace = current_scope[str]->clone();
				newNode = func_workspace->eval_const_function(this);
				delete func_workspace;
				goto apply_newNode;
			}

			if (in_param)
				log_error("Non-constant function call in constant expression at %s:%d.\n", filename.c_str(), linenum);
			if (require_const_eval)
				log_error("Function %s can only be called with constant arguments at %s:%d.\n", str.c_str(), filename.c_str(), linenum);
		}

		AstNode *decl = current_scope[str];
		std::stringstream sstr;
		sstr << "$func$" << str << "$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++) << "$";
		std::string prefix = sstr.str();

		size_t arg_count = 0;
		std::map<std::string, std::string> replace_rules;

		if (current_block == NULL)
		{
			assert(type == AST_FCALL);

			AstNode *wire = NULL;
			for (auto child : decl->children)
				if (child->type == AST_WIRE && child->str == str)
					wire = child->clone();
			assert(wire != NULL);

			wire->str = prefix + str;
			wire->port_id = 0;
			wire->is_input = false;
			wire->is_output = false;

			current_ast_mod->children.push_back(wire);
			while (wire->simplify(true, false, false, 1, -1, false, false)) { }

			AstNode *lvalue = new AstNode(AST_IDENTIFIER);
			lvalue->str = wire->str;

			AstNode *always = new AstNode(AST_ALWAYS, new AstNode(AST_BLOCK,
					new AstNode(AST_ASSIGN_EQ, lvalue, clone())));
			current_ast_mod->children.push_back(always);

			goto replace_fcall_with_id;
		}

		for (auto child : decl->children)
		{
			if (child->type == AST_WIRE)
			{
				AstNode *wire = child->clone();
				wire->str = prefix + wire->str;
				wire->port_id = 0;
				wire->is_input = false;
				wire->is_output = false;
				current_ast_mod->children.push_back(wire);
				while (wire->simplify(true, false, false, 1, -1, false, false)) { }

				replace_rules[child->str] = wire->str;

				if (child->is_input && arg_count < children.size())
				{
					AstNode *arg = children[arg_count++]->clone();
					AstNode *wire_id = new AstNode(AST_IDENTIFIER);
					wire_id->str = wire->str;
					AstNode *assign = new AstNode(AST_ASSIGN_EQ, wire_id, arg);

					for (auto it = current_block->children.begin(); it != current_block->children.end(); it++) {
						if (*it != current_block_child)
							continue;
						current_block->children.insert(it, assign);
						break;
					}
				}
			}
			else
			{
				AstNode *stmt = child->clone();
				stmt->replace_ids(replace_rules);

				for (auto it = current_block->children.begin(); it != current_block->children.end(); it++) {
					if (*it != current_block_child)
						continue;
					current_block->children.insert(it, stmt);
					break;
				}
			}
		}

	replace_fcall_with_id:
		if (type == AST_FCALL) {
			delete_children();
			type = AST_IDENTIFIER;
			str = prefix + str;
		}
		if (type == AST_TCALL)
			str = "";
		did_something = true;
	}

	// perform const folding when activated
	if (const_fold && newNode == NULL)
	{
		bool string_op;
		std::vector<RTLIL::State> tmp_bits;
		RTLIL::Const (*const_func)(const RTLIL::Const&, const RTLIL::Const&, bool, bool, int);
		RTLIL::Const dummy_arg;

		switch (type)
		{
		case AST_IDENTIFIER:
			if (current_scope.count(str) > 0 && (current_scope[str]->type == AST_PARAMETER || current_scope[str]->type == AST_LOCALPARAM)) {
				if (current_scope[str]->children[0]->type == AST_CONSTANT) {
					if (children.size() != 0 && children[0]->type == AST_RANGE && children[0]->range_valid) {
						std::vector<RTLIL::State> data;
						for (int i = children[0]->range_right; i <= children[0]->range_left; i++)
							data.push_back(current_scope[str]->children[0]->bits[i]);
						newNode = mkconst_bits(data, false);
					} else
					if (children.size() == 0)
						newNode = current_scope[str]->children[0]->clone();
				} else
				if (current_scope[str]->children[0]->isConst())
					newNode = current_scope[str]->children[0]->clone();
			}
			else if (at_zero && current_scope.count(str) > 0 && (current_scope[str]->type == AST_WIRE || current_scope[str]->type == AST_AUTOWIRE)) {
				newNode = mkconst_int(0, sign_hint, width_hint);
			}
			break;
		case AST_BIT_NOT:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = RTLIL::const_not(children[0]->bitsAsConst(width_hint, sign_hint), dummy_arg, sign_hint, false, width_hint);
				newNode = mkconst_bits(y.bits, sign_hint);
			}
			break;
		case AST_TO_SIGNED:
		case AST_TO_UNSIGNED:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = children[0]->bitsAsConst(width_hint, sign_hint);
				newNode = mkconst_bits(y.bits, type == AST_TO_SIGNED);
			}
			break;
		if (0) { case AST_BIT_AND:  const_func = RTLIL::const_and;  }
		if (0) { case AST_BIT_OR:   const_func = RTLIL::const_or;   }
		if (0) { case AST_BIT_XOR:  const_func = RTLIL::const_xor;  }
		if (0) { case AST_BIT_XNOR: const_func = RTLIL::const_xnor; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						children[1]->bitsAsConst(width_hint, sign_hint), sign_hint, sign_hint, width_hint);
				newNode = mkconst_bits(y.bits, sign_hint);
			}
			break;
		if (0) { case AST_REDUCE_AND:  const_func = RTLIL::const_reduce_and;  }
		if (0) { case AST_REDUCE_OR:   const_func = RTLIL::const_reduce_or;   }
		if (0) { case AST_REDUCE_XOR:  const_func = RTLIL::const_reduce_xor;  }
		if (0) { case AST_REDUCE_XNOR: const_func = RTLIL::const_reduce_xnor; }
		if (0) { case AST_REDUCE_BOOL: const_func = RTLIL::const_reduce_bool; }
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(RTLIL::Const(children[0]->bits), dummy_arg, false, false, -1);
				newNode = mkconst_bits(y.bits, false);
			}
			break;
		case AST_LOGIC_NOT:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = RTLIL::const_logic_not(RTLIL::Const(children[0]->bits), dummy_arg, children[0]->is_signed, false, -1);
				newNode = mkconst_bits(y.bits, false);
			} else
			if (children[0]->isConst()) {
				newNode = mkconst_int(children[0]->asReal(sign_hint) == 0, false, 1);
			}
			break;
		if (0) { case AST_LOGIC_AND: const_func = RTLIL::const_logic_and; }
		if (0) { case AST_LOGIC_OR:  const_func = RTLIL::const_logic_or;  }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(RTLIL::Const(children[0]->bits), RTLIL::Const(children[1]->bits),
						children[0]->is_signed, children[1]->is_signed, -1);
				newNode = mkconst_bits(y.bits, false);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				if (type == AST_LOGIC_AND)
					newNode = mkconst_int((children[0]->asReal(sign_hint) != 0) && (children[1]->asReal(sign_hint) != 0), false, 1);
				else
					newNode = mkconst_int((children[0]->asReal(sign_hint) != 0) || (children[1]->asReal(sign_hint) != 0), false, 1);
			}
			break;
		if (0) { case AST_SHIFT_LEFT:   const_func = RTLIL::const_shl;  }
		if (0) { case AST_SHIFT_RIGHT:  const_func = RTLIL::const_shr;  }
		if (0) { case AST_SHIFT_SLEFT:  const_func = RTLIL::const_sshl; }
		if (0) { case AST_SHIFT_SRIGHT: const_func = RTLIL::const_sshr; }
		if (0) { case AST_POW:          const_func = RTLIL::const_pow; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						RTLIL::Const(children[1]->bits), sign_hint, type == AST_POW ? children[1]->is_signed : false, width_hint);
				newNode = mkconst_bits(y.bits, sign_hint);
			} else
			if (type == AST_POW && children[0]->isConst() && children[1]->isConst()) {
				newNode = new AstNode(AST_REALVALUE);
				newNode->realvalue = pow(children[0]->asReal(sign_hint), children[1]->asReal(sign_hint));
			}
			break;
		if (0) { case AST_LT:  const_func = RTLIL::const_lt; }
		if (0) { case AST_LE:  const_func = RTLIL::const_le; }
		if (0) { case AST_EQ:  const_func = RTLIL::const_eq; }
		if (0) { case AST_NE:  const_func = RTLIL::const_ne; }
		if (0) { case AST_EQX: const_func = RTLIL::const_eqx; }
		if (0) { case AST_NEX: const_func = RTLIL::const_nex; }
		if (0) { case AST_GE:  const_func = RTLIL::const_ge; }
		if (0) { case AST_GT:  const_func = RTLIL::const_gt; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				int cmp_width = std::max(children[0]->bits.size(), children[1]->bits.size());
				bool cmp_signed = children[0]->is_signed && children[1]->is_signed;
				RTLIL::Const y = const_func(children[0]->bitsAsConst(cmp_width, cmp_signed),
						children[1]->bitsAsConst(cmp_width, cmp_signed), cmp_signed, cmp_signed, 1);
				newNode = mkconst_bits(y.bits, false);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				bool cmp_signed = (children[0]->type == AST_REALVALUE || children[0]->is_signed) && (children[1]->type == AST_REALVALUE || children[1]->is_signed);
				switch (type) {
				case AST_LT:  newNode = mkconst_int(children[0]->asReal(cmp_signed) <  children[1]->asReal(cmp_signed), false, 1); break;
				case AST_LE:  newNode = mkconst_int(children[0]->asReal(cmp_signed) <= children[1]->asReal(cmp_signed), false, 1); break;
				case AST_EQ:  newNode = mkconst_int(children[0]->asReal(cmp_signed) == children[1]->asReal(cmp_signed), false, 1); break;
				case AST_NE:  newNode = mkconst_int(children[0]->asReal(cmp_signed) != children[1]->asReal(cmp_signed), false, 1); break;
				case AST_EQX: newNode = mkconst_int(children[0]->asReal(cmp_signed) == children[1]->asReal(cmp_signed), false, 1); break;
				case AST_NEX: newNode = mkconst_int(children[0]->asReal(cmp_signed) != children[1]->asReal(cmp_signed), false, 1); break;
				case AST_GE:  newNode = mkconst_int(children[0]->asReal(cmp_signed) >= children[1]->asReal(cmp_signed), false, 1); break;
				case AST_GT:  newNode = mkconst_int(children[0]->asReal(cmp_signed) >  children[1]->asReal(cmp_signed), false, 1); break;
				default: log_abort();
				}
			}
			break;
		if (0) { case AST_ADD: const_func = RTLIL::const_add; }
		if (0) { case AST_SUB: const_func = RTLIL::const_sub; }
		if (0) { case AST_MUL: const_func = RTLIL::const_mul; }
		if (0) { case AST_DIV: const_func = RTLIL::const_div; }
		if (0) { case AST_MOD: const_func = RTLIL::const_mod; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						children[1]->bitsAsConst(width_hint, sign_hint), sign_hint, sign_hint, width_hint);
				newNode = mkconst_bits(y.bits, sign_hint);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				newNode = new AstNode(AST_REALVALUE);
				switch (type) {
				case AST_ADD: newNode->realvalue = children[0]->asReal(sign_hint) + children[1]->asReal(sign_hint); break;
				case AST_SUB: newNode->realvalue = children[0]->asReal(sign_hint) - children[1]->asReal(sign_hint); break;
				case AST_MUL: newNode->realvalue = children[0]->asReal(sign_hint) * children[1]->asReal(sign_hint); break;
				case AST_DIV: newNode->realvalue = children[0]->asReal(sign_hint) / children[1]->asReal(sign_hint); break;
				case AST_MOD: newNode->realvalue = fmod(children[0]->asReal(sign_hint), children[1]->asReal(sign_hint)); break;
				default: log_abort();
				}
			}
			break;
		if (0) { case AST_POS: const_func = RTLIL::const_pos; }
		if (0) { case AST_NEG: const_func = RTLIL::const_neg; }
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint), dummy_arg, sign_hint, false, width_hint);
				newNode = mkconst_bits(y.bits, sign_hint);
			} else
			if (children[0]->isConst()) {
				newNode = new AstNode(AST_REALVALUE);
				if (type == AST_POS)
					newNode->realvalue = +children[0]->asReal(sign_hint);
				else
					newNode->realvalue = -children[0]->asReal(sign_hint);
			}
			break;
		case AST_TERNARY:
			if (children[0]->isConst())
			{
				bool found_sure_true = false;
				bool found_maybe_true = false;

				if (children[0]->type == AST_CONSTANT)
					for (auto &bit : children[0]->bits) {
						if (bit == RTLIL::State::S1)
							found_sure_true = true;
						if (bit > RTLIL::State::S1)
							found_maybe_true = true;
					}
				else
					found_sure_true = children[0]->asReal(sign_hint) != 0;

				AstNode *choice = NULL, *not_choice = NULL;
				if (found_sure_true)
					choice = children[1], not_choice = children[2];
				else if (!found_maybe_true)
					choice = children[2], not_choice = children[1];

				if (choice != NULL) {
					if (choice->type == AST_CONSTANT) {
						int other_width_hint = width_hint;
						bool other_sign_hint = sign_hint, other_real = false;
						not_choice->detectSignWidth(other_width_hint, other_sign_hint, &other_real);
						if (other_real) {
							newNode = new AstNode(AST_REALVALUE);
							choice->detectSignWidth(width_hint, sign_hint);
							newNode->realvalue = choice->asReal(sign_hint);
						} else {
							RTLIL::Const y = choice->bitsAsConst(width_hint, sign_hint);
							if (choice->is_string && y.bits.size() % 8 == 0 && sign_hint == false)
								newNode = mkconst_str(y.bits);
							else
								newNode = mkconst_bits(y.bits, sign_hint);
						}
					} else
					if (choice->isConst()) {
						newNode = choice->clone();
					}
				} else if (children[1]->type == AST_CONSTANT && children[2]->type == AST_CONSTANT) {
					RTLIL::Const a = children[1]->bitsAsConst(width_hint, sign_hint);
					RTLIL::Const b = children[2]->bitsAsConst(width_hint, sign_hint);
					assert(a.bits.size() == b.bits.size());
					for (size_t i = 0; i < a.bits.size(); i++)
						if (a.bits[i] != b.bits[i])
							a.bits[i] = RTLIL::State::Sx;
					newNode = mkconst_bits(a.bits, sign_hint);
				} else if (children[1]->isConst() && children[2]->isConst()) {
					newNode = new AstNode(AST_REALVALUE);
					if (children[1]->asReal(sign_hint) == children[2]->asReal(sign_hint))
						newNode->realvalue = children[1]->asReal(sign_hint);
					else
						// IEEE Std 1800-2012 Sec. 11.4.11 states that the entry in Table 7-1 for
						// the data type in question should be returned if the ?: is ambiguous. The
						// value in Table 7-1 for the 'real' type is 0.0.
						newNode->realvalue = 0.0;
				}
			}
			break;
		case AST_CONCAT:
			string_op = !children.empty();
			for (auto it = children.begin(); it != children.end(); it++) {
				if ((*it)->type != AST_CONSTANT)
					goto not_const;
				if (!(*it)->is_string)
					string_op = false;
				tmp_bits.insert(tmp_bits.end(), (*it)->bits.begin(), (*it)->bits.end());
			}
			newNode = string_op ? mkconst_str(tmp_bits) : mkconst_bits(tmp_bits, false);
			break;
		case AST_REPLICATE:
			if (children.at(0)->type != AST_CONSTANT || children.at(1)->type != AST_CONSTANT)
				goto not_const;
			for (int i = 0; i < children[0]->bitsAsConst().as_int(); i++)
				tmp_bits.insert(tmp_bits.end(), children.at(1)->bits.begin(), children.at(1)->bits.end());
			newNode = children.at(1)->is_string ? mkconst_str(tmp_bits) : mkconst_bits(tmp_bits, false);
			break;
		default:
		not_const:
			break;
		}
	}

	// if any of the above set 'newNode' -> use 'newNode' as template to update 'this'
	if (newNode) {
apply_newNode:
		// fprintf(stderr, "----\n");
		// dumpAst(stderr, "- ");
		// newNode->dumpAst(stderr, "+ ");
		assert(newNode != NULL);
		newNode->filename = filename;
		newNode->linenum = linenum;
		newNode->cloneInto(this);
		delete newNode;
		did_something = true;
	}

	if (!did_something)
		basic_prep = true;

	return did_something;
}

static void replace_result_wire_name_in_function(AstNode *node, std::string &from, std::string &to)
{
	for (auto &it : node->children)
		replace_result_wire_name_in_function(it, from, to);
	if (node->str == from)
		node->str = to;
}

// annotate the names of all wires and other named objects in a generate block
void AstNode::expand_genblock(std::string index_var, std::string prefix, std::map<std::string, std::string> &name_map)
{
	if (!index_var.empty() && type == AST_IDENTIFIER && str == index_var) {
		current_scope[index_var]->children[0]->cloneInto(this);
		return;
	}

	if ((type == AST_IDENTIFIER || type == AST_FCALL || type == AST_TCALL) && name_map.count(str) > 0)
		str = name_map[str];

	std::map<std::string, std::string> backup_name_map;

	for (size_t i = 0; i < children.size(); i++) {
		AstNode *child = children[i];
		if (child->type == AST_WIRE || child->type == AST_MEMORY || child->type == AST_PARAMETER || child->type == AST_LOCALPARAM ||
				child->type == AST_FUNCTION || child->type == AST_TASK || child->type == AST_CELL) {
			if (backup_name_map.size() == 0)
				backup_name_map = name_map;
			std::string new_name = prefix[0] == '\\' ? prefix.substr(1) : prefix;
			size_t pos = child->str.rfind('.');
			if (pos == std::string::npos)
				pos = child->str[0] == '\\' ? 1 : 0;
			else
				pos = pos + 1;
			new_name = child->str.substr(0, pos) + new_name + child->str.substr(pos);
			if (new_name[0] != '$' && new_name[0] != '\\')
				new_name = prefix[0] + new_name;
			name_map[child->str] = new_name;
			if (child->type == AST_FUNCTION)
				replace_result_wire_name_in_function(child, child->str, new_name);
			else
				child->str = new_name;
			current_scope[new_name] = child;
		}
	}

	for (size_t i = 0; i < children.size(); i++) {
		AstNode *child = children[i];
		if (child->type != AST_FUNCTION && child->type != AST_TASK && child->type != AST_PREFIX)
			child->expand_genblock(index_var, prefix, name_map);
	}

	if (backup_name_map.size() > 0)
		name_map.swap(backup_name_map);
}

// rename stuff (used when tasks of functions are instanciated)
void AstNode::replace_ids(std::map<std::string, std::string> &rules)
{
	if (type == AST_IDENTIFIER && rules.count(str) > 0)
		str = rules[str];
	for (auto child : children)
		child->replace_ids(rules);
}

// helper function for mem2reg_as_needed_pass1
static void mark_memories_assign_lhs_complex(std::map<AstNode*, std::set<std::string>> &mem2reg_places,
		std::map<AstNode*, uint32_t> &mem2reg_candidates, AstNode *that)
{
	for (auto &child : that->children)
		mark_memories_assign_lhs_complex(mem2reg_places, mem2reg_candidates, child);

	if (that->type == AST_IDENTIFIER && that->id2ast && that->id2ast->type == AST_MEMORY) {
		AstNode *mem = that->id2ast;
		if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_CMPLX_LHS))
			mem2reg_places[mem].insert(stringf("%s:%d", that->filename.c_str(), that->linenum));
		mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_CMPLX_LHS;
	}
}

// find memories that should be replaced by registers
void AstNode::mem2reg_as_needed_pass1(std::map<AstNode*, std::set<std::string>> &mem2reg_places,
		std::map<AstNode*, uint32_t> &mem2reg_candidates, std::map<AstNode*, uint32_t> &proc_flags, uint32_t &flags)
{
	uint32_t children_flags = 0;
	int ignore_children_counter = 0;

	if (type == AST_ASSIGN || type == AST_ASSIGN_LE || type == AST_ASSIGN_EQ)
	{
		// mark all memories that are used in a complex expression on the left side of an assignment
		for (auto &lhs_child : children[0]->children)
			mark_memories_assign_lhs_complex(mem2reg_places, mem2reg_candidates, lhs_child);

		if (children[0]->type == AST_IDENTIFIER && children[0]->id2ast && children[0]->id2ast->type == AST_MEMORY)
		{
			AstNode *mem = children[0]->id2ast;

			// activate mem2reg if this is assigned in an async proc
			if (flags & AstNode::MEM2REG_FL_ASYNC) {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_ASYNC))
					mem2reg_places[mem].insert(stringf("%s:%d", filename.c_str(), linenum));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_ASYNC;
			}

			// remember if this is assigned blocking (=)
			if (type == AST_ASSIGN_EQ) {
				if (!(proc_flags[mem] & AstNode::MEM2REG_FL_EQ1))
					mem2reg_places[mem].insert(stringf("%s:%d", filename.c_str(), linenum));
				proc_flags[mem] |= AstNode::MEM2REG_FL_EQ1;
			}

			// remember where this is
			if (flags & MEM2REG_FL_INIT) {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_INIT))
					mem2reg_places[mem].insert(stringf("%s:%d", filename.c_str(), linenum));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_INIT;
			} else {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_ELSE))
					mem2reg_places[mem].insert(stringf("%s:%d", filename.c_str(), linenum));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_ELSE;
			}
		}

		ignore_children_counter = 1;
	}

	if (type == AST_IDENTIFIER && id2ast && id2ast->type == AST_MEMORY)
	{
		AstNode *mem = id2ast;

		// flag if used after blocking assignment (in same proc)
		if ((proc_flags[mem] & AstNode::MEM2REG_FL_EQ1) && !(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_EQ2)) {
			mem2reg_places[mem].insert(stringf("%s:%d", filename.c_str(), linenum));
			mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_EQ2;
		}
	}

	// also activate if requested, either by using mem2reg attribute or by declaring array as 'wire' instead of 'reg'
	if (type == AST_MEMORY && (get_bool_attribute("\\mem2reg") || (flags & AstNode::MEM2REG_FL_ALL) || !is_reg))
		mem2reg_candidates[this] |= AstNode::MEM2REG_FL_FORCED;

	if (type == AST_MODULE && get_bool_attribute("\\mem2reg"))
		children_flags |= AstNode::MEM2REG_FL_ALL;

	std::map<AstNode*, uint32_t> *proc_flags_p = NULL;

	if (type == AST_ALWAYS) {
		int count_edge_events = 0;
		for (auto child : children)
			if (child->type == AST_POSEDGE || child->type == AST_NEGEDGE)
				count_edge_events++;
		if (count_edge_events != 1)
			children_flags |= AstNode::MEM2REG_FL_ASYNC;
		proc_flags_p = new std::map<AstNode*, uint32_t>;
	}

	if (type == AST_INITIAL) {
		children_flags |= AstNode::MEM2REG_FL_INIT;
		proc_flags_p = new std::map<AstNode*, uint32_t>;
	}

	uint32_t backup_flags = flags;
	flags |= children_flags;
	assert((flags & ~0x000000ff) == 0);

	for (auto child : children)
		if (ignore_children_counter > 0)
			ignore_children_counter--;
		else if (proc_flags_p)
			child->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, *proc_flags_p, flags);
		else
			child->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, proc_flags, flags);

	flags &= ~children_flags | backup_flags;

	if (proc_flags_p) {
		for (auto it : *proc_flags_p)
			log_assert((it.second & ~0xff000000) == 0);
		delete proc_flags_p;
	}
}

// actually replace memories with registers
void AstNode::mem2reg_as_needed_pass2(std::set<AstNode*> &mem2reg_set, AstNode *mod, AstNode *block)
{
	if (type == AST_BLOCK)
		block = this;

	if ((type == AST_ASSIGN_LE || type == AST_ASSIGN_EQ) && block != NULL && children[0]->id2ast &&
			mem2reg_set.count(children[0]->id2ast) > 0 && children[0]->children[0]->children[0]->type != AST_CONSTANT)
	{
		std::stringstream sstr;
		sstr << "$mem2reg_wr$" << children[0]->str << "$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
		std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA";

		int mem_width, mem_size, addr_bits;
		children[0]->id2ast->meminfo(mem_width, mem_size, addr_bits);

		AstNode *wire_addr = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(addr_bits-1, true), mkconst_int(0, true)));
		wire_addr->str = id_addr;
		wire_addr->is_reg = true;
		wire_addr->attributes["\\nosync"] = AstNode::mkconst_int(1, false);
		mod->children.push_back(wire_addr);
		while (wire_addr->simplify(true, false, false, 1, -1, false, false)) { }

		AstNode *wire_data = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(mem_width-1, true), mkconst_int(0, true)));
		wire_data->str = id_data;
		wire_data->is_reg = true;
		wire_data->attributes["\\nosync"] = AstNode::mkconst_int(1, false);
		mod->children.push_back(wire_data);
		while (wire_data->simplify(true, false, false, 1, -1, false, false)) { }

		assert(block != NULL);
		size_t assign_idx = 0;
		while (assign_idx < block->children.size() && block->children[assign_idx] != this)
			assign_idx++;
		assert(assign_idx < block->children.size());

		AstNode *assign_addr = new AstNode(AST_ASSIGN_EQ, new AstNode(AST_IDENTIFIER), children[0]->children[0]->children[0]->clone());
		assign_addr->children[0]->str = id_addr;
		block->children.insert(block->children.begin()+assign_idx+1, assign_addr);

		AstNode *case_node = new AstNode(AST_CASE, new AstNode(AST_IDENTIFIER));
		case_node->children[0]->str = id_addr;
		for (int i = 0; i < mem_size; i++) {
			if (children[0]->children[0]->children[0]->type == AST_CONSTANT && int(children[0]->children[0]->children[0]->integer) != i)
				continue;
			AstNode *cond_node = new AstNode(AST_COND, AstNode::mkconst_int(i, false, addr_bits), new AstNode(AST_BLOCK));
			AstNode *assign_reg = new AstNode(type, new AstNode(AST_IDENTIFIER), new AstNode(AST_IDENTIFIER));
			assign_reg->children[0]->str = stringf("%s[%d]", children[0]->str.c_str(), i);
			assign_reg->children[1]->str = id_data;
			cond_node->children[1]->children.push_back(assign_reg);
			case_node->children.push_back(cond_node);
		}
		block->children.insert(block->children.begin()+assign_idx+2, case_node);

		children[0]->delete_children();
		children[0]->range_valid = false;
		children[0]->id2ast = NULL;
		children[0]->str = id_data;
		type = AST_ASSIGN_EQ;
	}

	if (type == AST_IDENTIFIER && id2ast && mem2reg_set.count(id2ast) > 0)
	{
		if (children[0]->children[0]->type == AST_CONSTANT)
		{
			int id = children[0]->children[0]->integer;
			str = stringf("%s[%d]", str.c_str(), id);

			delete_children();
			range_valid = false;
			id2ast = NULL;
		}
		else
		{
			std::stringstream sstr;
			sstr << "$mem2reg_rd$" << children[0]->str << "$" << filename << ":" << linenum << "$" << (RTLIL::autoidx++);
			std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA";

			int mem_width, mem_size, addr_bits;
			id2ast->meminfo(mem_width, mem_size, addr_bits);

			AstNode *wire_addr = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(addr_bits-1, true), mkconst_int(0, true)));
			wire_addr->str = id_addr;
			wire_addr->is_reg = true;
			if (block)
				wire_addr->attributes["\\nosync"] = AstNode::mkconst_int(1, false);
			mod->children.push_back(wire_addr);
			while (wire_addr->simplify(true, false, false, 1, -1, false, false)) { }

			AstNode *wire_data = new AstNode(AST_WIRE, new AstNode(AST_RANGE, mkconst_int(mem_width-1, true), mkconst_int(0, true)));
			wire_data->str = id_data;
			wire_data->is_reg = true;
			if (block)
				wire_data->attributes["\\nosync"] = AstNode::mkconst_int(1, false);
			mod->children.push_back(wire_data);
			while (wire_data->simplify(true, false, false, 1, -1, false, false)) { }

			AstNode *assign_addr = new AstNode(block ? AST_ASSIGN_EQ : AST_ASSIGN, new AstNode(AST_IDENTIFIER), children[0]->children[0]->clone());
			assign_addr->children[0]->str = id_addr;

			AstNode *case_node = new AstNode(AST_CASE, new AstNode(AST_IDENTIFIER));
			case_node->children[0]->str = id_addr;

			for (int i = 0; i < mem_size; i++) {
				if (children[0]->children[0]->type == AST_CONSTANT && int(children[0]->children[0]->integer) != i)
					continue;
				AstNode *cond_node = new AstNode(AST_COND, AstNode::mkconst_int(i, false, addr_bits), new AstNode(AST_BLOCK));
				AstNode *assign_reg = new AstNode(AST_ASSIGN_EQ, new AstNode(AST_IDENTIFIER), new AstNode(AST_IDENTIFIER));
				assign_reg->children[0]->str = id_data;
				assign_reg->children[1]->str = stringf("%s[%d]", str.c_str(), i);
				cond_node->children[1]->children.push_back(assign_reg);
				case_node->children.push_back(cond_node);
			}

			std::vector<RTLIL::State> x_bits;
			for (int i = 0; i < mem_width; i++)
				x_bits.push_back(RTLIL::State::Sx);

			AstNode *cond_node = new AstNode(AST_COND, new AstNode(AST_DEFAULT), new AstNode(AST_BLOCK));
			AstNode *assign_reg = new AstNode(AST_ASSIGN_EQ, new AstNode(AST_IDENTIFIER), AstNode::mkconst_bits(x_bits, false));
			assign_reg->children[0]->str = id_data;
			cond_node->children[1]->children.push_back(assign_reg);
			case_node->children.push_back(cond_node);

			if (block)
			{
				size_t assign_idx = 0;
				while (assign_idx < block->children.size() && !block->children[assign_idx]->contains(this))
					assign_idx++;
				assert(assign_idx < block->children.size());
				block->children.insert(block->children.begin()+assign_idx, case_node);
				block->children.insert(block->children.begin()+assign_idx, assign_addr);
			}
			else
			{
				AstNode *proc = new AstNode(AST_ALWAYS, new AstNode(AST_BLOCK));
				proc->children[0]->children.push_back(case_node);
				mod->children.push_back(proc);
				mod->children.push_back(assign_addr);
			}

			delete_children();
			range_valid = false;
			id2ast = NULL;
			str = id_data;
		}
	}

	assert(id2ast == NULL || mem2reg_set.count(id2ast) == 0);

	auto children_list = children;
	for (size_t i = 0; i < children_list.size(); i++)
		children_list[i]->mem2reg_as_needed_pass2(mem2reg_set, mod, block);
}

// calulate memory dimensions
void AstNode::meminfo(int &mem_width, int &mem_size, int &addr_bits)
{
	assert(type == AST_MEMORY);

	mem_width = children[0]->range_left - children[0]->range_right + 1;
	mem_size = children[1]->range_left - children[1]->range_right;

	if (mem_size < 0)
		mem_size *= -1;
	mem_size += std::min(children[1]->range_left, children[1]->range_right) + 1;

	addr_bits = 1;
	while ((1 << addr_bits) < mem_size)
		addr_bits++;
}

bool AstNode::has_const_only_constructs(bool &recommend_const_eval)
{
	if (type == AST_FOR)
		recommend_const_eval = true;
	if (type == AST_WHILE || type == AST_REPEAT)
		return true;
	if (type == AST_FCALL && current_scope.count(str))
		if (current_scope[str]->has_const_only_constructs(recommend_const_eval))
			return true;
	for (auto child : children)
		if (child->AstNode::has_const_only_constructs(recommend_const_eval))
			return true;
	return false;
}

// helper function for AstNode::eval_const_function()
void AstNode::replace_variables(std::map<std::string, AstNode::varinfo_t> &variables, AstNode *fcall)
{
	if (type == AST_IDENTIFIER && variables.count(str)) {
		int offset = variables.at(str).offset, width = variables.at(str).val.bits.size();
		if (!children.empty()) {
			if (children.size() != 1 || children.at(0)->type != AST_RANGE)
				log_error("Memory access in constant function is not supported in %s:%d (called from %s:%d).\n",
						filename.c_str(), linenum, fcall->filename.c_str(), fcall->linenum);
			children.at(0)->replace_variables(variables, fcall);
			while (simplify(true, false, false, 1, -1, false, true)) { }
			if (!children.at(0)->range_valid)
				log_error("Non-constant range in %s:%d (called from %s:%d).\n",
						filename.c_str(), linenum, fcall->filename.c_str(), fcall->linenum);
			offset = std::min(children.at(0)->range_left, children.at(0)->range_right);
			width = std::min(std::abs(children.at(0)->range_left - children.at(0)->range_right) + 1, width);
		}
		offset -= variables.at(str).offset;
		std::vector<RTLIL::State> &var_bits = variables.at(str).val.bits;
		std::vector<RTLIL::State> new_bits(var_bits.begin() + offset, var_bits.begin() + offset + width);
		AstNode *newNode = mkconst_bits(new_bits, variables.at(str).is_signed);
		newNode->cloneInto(this);
		delete newNode;
		return;
	}

	for (auto &child : children)
		child->replace_variables(variables, fcall);
}

// evaluate functions with all-const arguments
AstNode *AstNode::eval_const_function(AstNode *fcall)
{
	std::map<std::string, AstNode*> backup_scope;
	std::map<std::string, AstNode::varinfo_t> variables;
	bool delete_temp_block = false;
	AstNode *block = NULL;

	size_t argidx = 0;
	for (auto child : children)
	{
		if (child->type == AST_BLOCK)
		{
			log_assert(block == NULL);
			block = child;
			continue;
		}

		if (child->type == AST_WIRE)
		{
			while (child->simplify(true, false, false, 1, -1, false, true)) { }
			if (!child->range_valid)
				log_error("Can't determine size of variable %s in %s:%d (called from %s:%d).\n",
						child->str.c_str(), child->filename.c_str(), child->linenum, fcall->filename.c_str(), fcall->linenum);
			variables[child->str].val = RTLIL::Const(RTLIL::State::Sx, abs(child->range_left - child->range_right)+1);
			variables[child->str].offset = std::min(child->range_left, child->range_right);
			variables[child->str].is_signed = child->is_signed;
			if (child->is_input && argidx < fcall->children.size())
				variables[child->str].val = fcall->children.at(argidx++)->bitsAsConst(variables[child->str].val.bits.size());
			backup_scope[child->str] = current_scope[child->str];
			current_scope[child->str] = child;
			continue;
		}

		log_assert(block == NULL);
		delete_temp_block = true;
		block = new AstNode(AST_BLOCK);
		block->children.push_back(child->clone());
	}

	log_assert(block != NULL);
	log_assert(variables.count(str));

	while (!block->children.empty())
	{
		AstNode *stmt = block->children.front();

#if 0
		log("-----------------------------------\n");
		for (auto &it : variables)
			log("%20s %40s\n", it.first.c_str(), log_signal(it.second.val));
		stmt->dumpAst(NULL, "stmt> ");
#endif

		if (stmt->type == AST_ASSIGN_EQ)
		{
			stmt->children.at(1)->replace_variables(variables, fcall);
			while (stmt->simplify(true, false, false, 1, -1, false, true)) { }

			if (stmt->type != AST_ASSIGN_EQ)
				continue;

			if (stmt->children.at(1)->type != AST_CONSTANT)
				log_error("Non-constant expression in constant function at %s:%d (called from %s:%d). X\n",
						stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

			if (stmt->children.at(0)->type != AST_IDENTIFIER)
				log_error("Unsupported composite left hand side in constant function at %s:%d (called from %s:%d).\n",
						stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

			if (!variables.count(stmt->children.at(0)->str))
				log_error("Assignment to non-local variable in constant function at %s:%d (called from %s:%d).\n",
						stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

			if (stmt->children.at(0)->children.empty()) {
				variables[stmt->children.at(0)->str].val = stmt->children.at(1)->bitsAsConst(variables[stmt->children.at(0)->str].val.bits.size());
			} else {
				AstNode *range = stmt->children.at(0)->children.at(0);
				if (!range->range_valid)
					log_error("Non-constant range in %s:%d (called from %s:%d).\n",
							range->filename.c_str(), range->linenum, fcall->filename.c_str(), fcall->linenum);
				int offset = std::min(range->range_left, range->range_right);
				int width = std::min(std::abs(range->range_left - range->range_right) + 1, width);
				varinfo_t &v = variables[stmt->children.at(0)->str];
				RTLIL::Const r = stmt->children.at(1)->bitsAsConst(v.val.bits.size());
				for (int i = 0; i < width; i++)
					v.val.bits.at(i+offset-v.offset) = r.bits.at(i);
			}

			delete block->children.front();
			block->children.erase(block->children.begin());
			continue;
		}

		if (stmt->type == AST_FOR)
		{
			block->children.insert(block->children.begin(), stmt->children.at(0));
			stmt->children.at(3)->children.push_back(stmt->children.at(2));
			stmt->children.erase(stmt->children.begin() + 2);
			stmt->children.erase(stmt->children.begin());
			stmt->type = AST_WHILE;
			continue;
		}

		if (stmt->type == AST_WHILE)
		{
			AstNode *cond = stmt->children.at(0)->clone();
			cond->replace_variables(variables, fcall);
			while (cond->simplify(true, false, false, 1, -1, false, true)) { }

			if (cond->type != AST_CONSTANT)
				log_error("Non-constant expression in constant function at %s:%d (called from %s:%d).\n",
						stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

			if (cond->asBool()) {
				block->children.insert(block->children.begin(), stmt->children.at(1)->clone());
			} else {
				delete block->children.front();
				block->children.erase(block->children.begin());
			}

			delete cond;
			continue;
		}

		if (stmt->type == AST_REPEAT)
		{
			AstNode *num = stmt->children.at(0)->clone();
			num->replace_variables(variables, fcall);
			while (num->simplify(true, false, false, 1, -1, false, true)) { }

			if (num->type != AST_CONSTANT)
				log_error("Non-constant expression in constant function at %s:%d (called from %s:%d).\n",
						stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

			block->children.erase(block->children.begin());
			for (int i = 0; i < num->bitsAsConst().as_int(); i++)
				block->children.insert(block->children.begin(), stmt->children.at(1)->clone());

			delete stmt;
			delete num;
			continue;
		}

		if (stmt->type == AST_CASE)
		{
			AstNode *expr = stmt->children.at(0)->clone();
			expr->replace_variables(variables, fcall);
			while (expr->simplify(true, false, false, 1, -1, false, true)) { }

			AstNode *sel_case = NULL;
			for (size_t i = 1; i < stmt->children.size(); i++)
			{
				bool found_match = false;
				log_assert(stmt->children.at(i)->type == AST_COND);

				if (stmt->children.at(i)->children.front()->type == AST_DEFAULT) {
					sel_case = stmt->children.at(i)->children.back();
					continue;
				}

				for (size_t j = 0; j+1 < stmt->children.at(i)->children.size() && !found_match; j++)
				{
					AstNode *cond = stmt->children.at(i)->children.at(j)->clone();
					cond->replace_variables(variables, fcall);

					cond = new AstNode(AST_EQ, expr->clone(), cond);
					while (cond->simplify(true, false, false, 1, -1, false, true)) { }

					if (cond->type != AST_CONSTANT)
						log_error("Non-constant expression in constant function at %s:%d (called from %s:%d).\n",
								stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);

					found_match = cond->asBool();
					delete cond;
				}

				if (found_match) {
					sel_case = stmt->children.at(i)->children.back();
					break;
				}
			}

			block->children.erase(block->children.begin());
			if (sel_case)
				block->children.insert(block->children.begin(), sel_case->clone());
			delete stmt;
			delete expr;
			continue;
		}

		if (stmt->type == AST_BLOCK)
		{
			block->children.erase(block->children.begin());
			block->children.insert(block->children.begin(), stmt->children.begin(), stmt->children.end());
			stmt->children.clear();
			delete stmt;
			continue;
		}

		log_error("Unsupported language construct in constant function at %s:%d (called from %s:%d).\n",
				stmt->filename.c_str(), stmt->linenum, fcall->filename.c_str(), fcall->linenum);
		log_abort();
	}

	if (delete_temp_block)
		delete block;

	for (auto &it : backup_scope)
		if (it.second == NULL)
			current_scope.erase(it.first);
		else
			current_scope[it.first] = it.second;

	return AstNode::mkconst_bits(variables.at(str).val.bits, variables.at(str).is_signed);
}

