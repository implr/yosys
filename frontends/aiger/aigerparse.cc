/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *                      Eddie Hung <eddie@fpgeh.com>
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
 */

// [[CITE]] The AIGER And-Inverter Graph (AIG) Format Version 20071012
// Armin Biere. The AIGER And-Inverter Graph (AIG) Format Version 20071012. Technical Report 07/1, October 2011, FMV Reports Series, Institute for Formal Models and Verification, Johannes Kepler University, Altenbergerstr. 69, 4040 Linz, Austria.
// http://fmv.jku.at/papers/Biere-FMV-TR-07-1.pdf

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "aigerparse.h"

YOSYS_NAMESPACE_BEGIN

#define log_debug log

static void parse_aiger_ascii(RTLIL::Module *module, std::istream &f, std::string clk_name);
static void parse_aiger_binary(RTLIL::Module *module, std::istream &f, std::string clk_name);

void parse_aiger(RTLIL::Design *design, std::istream &f, std::string clk_name)
{
    auto module = new RTLIL::Module;
    module->name = RTLIL::escape_id("aig"); // TODO: Name?
    if (design->module(module->name))
        log_error("Duplicate definition of module %s!\n", log_id(module->name));

    std::string header;
    f >> header;
    if (header == "aag")
        parse_aiger_ascii(module, f, clk_name);
    else if (header == "aig")
        parse_aiger_binary(module, f, clk_name);
    else
        log_error("Unsupported AIGER file!\n");

    module->fixup_ports();
    design->add(module);
}

static RTLIL::Wire* createWireIfNotExists(RTLIL::Module *module, unsigned literal)
{
    const unsigned variable = literal >> 1;
    const bool invert = literal & 1;
    RTLIL::IdString wire_name(stringf("\\n%d%s", variable, invert ? "_inv" : "")); // FIXME: is "_inv" the right suffix?
    RTLIL::Wire *wire = module->wire(wire_name);
    if (wire) return wire;
    log_debug("Creating %s\n", wire_name.c_str());
    wire = module->addWire(wire_name);
    if (!invert) return wire;
    RTLIL::IdString wire_inv_name(stringf("\\n%d", variable));
    RTLIL::Wire *wire_inv = module->wire(wire_inv_name);
    if (wire_inv) {
        if (module->cell(wire_inv_name)) return wire;
    }
    else {
        log_debug("Creating %s\n", wire_inv_name.c_str());
        wire_inv = module->addWire(wire_inv_name);
    }

    log_debug("Creating %s = ~%s\n", wire_name.c_str(), wire_inv_name.c_str());
    RTLIL::Cell *inv = module->addCell(stringf("\\n%d_not", variable), "$_NOT_"); // FIXME: is "_not" the right suffix?
    inv->setPort("\\A", wire_inv);
    inv->setPort("\\Y", wire);

    return wire;
}

static void parse_aiger_header(std::istream &f, unsigned &M, unsigned &I, unsigned &L, unsigned &O, unsigned &A, unsigned &B, unsigned &C, unsigned &J, unsigned &F)
{
    if (!(f >> M >> I >> L >> O >> A))
        log_error("Invalid AIGER header\n");
    for (auto &i : std::array<std::reference_wrapper<unsigned>,4>{B, C, J, F}) {
        if (f.peek() != ' ') break;
        if (!(f >> i))
            log_error("Invalid AIGER header\n");
    }

    std::string line;
    std::getline(f, line); // Ignore up to start of next line, as standard
                           // says anything that follows could be used for
                           // optional sections
    
    log_debug("M=%u I=%u L=%u O=%u A=%u B=%u C=%u J=%u F=%u\n", M, I, L, O, A, B, C, J, F);
}

static void parse_aiger_ascii(RTLIL::Module *module, std::istream &f, std::string clk_name)
{
    unsigned M, I, L, O, A;
    unsigned B=0, C=0, J=0, F=0; // Optional in AIGER 1.9
    parse_aiger_header(f, M, I, L, O, A, B, C, J, F);

    unsigned line_count = 1;
    std::string line;
    std::stringstream ss;

    unsigned l1, l2, l3;

    // Parse inputs
    std::vector<RTLIL::Wire*> inputs;
    for (unsigned i = 0; i < I; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an input!\n", line_count);
        log_debug("%d is an input\n", l1);
        log_assert(!(l1 & 1)); // TODO: Inputs can't be inverted?
        RTLIL::Wire *wire = createWireIfNotExists(module, l1);
        wire->port_input = true;
        inputs.push_back(wire);
    }

    // Parse latches
    std::vector<RTLIL::Wire*> latches;
    RTLIL::Wire *clk_wire = nullptr;
    if (L > 0) {
        RTLIL::IdString clk_id = RTLIL::escape_id(clk_name.c_str());
        clk_wire = module->wire(clk_id);
        log_assert(!clk_wire);
        log_debug("Creating %s\n", clk_id.c_str());
        clk_wire = module->addWire(clk_id);
        clk_wire->port_input = true;
    }
    for (unsigned i = 0; i < L; ++i, ++line_count) {
        if (!(f >> l1 >> l2))
            log_error("Line %u cannot be interpreted as a latch!\n", line_count);
        log_debug("%d %d is a latch\n", l1, l2);
        log_assert(!(l1 & 1)); // TODO: Latch outputs can't be inverted?
        RTLIL::Wire *q_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *d_wire = createWireIfNotExists(module, l2);

        module->addDff(NEW_ID, clk_wire, d_wire, q_wire);

        // Reset logic is optional in AIGER 1.9
        if (f.peek() == ' ') {
            if (!(f >> l3))
                log_error("Line %u cannot be interpreted as a latch!\n", line_count);

            if (l3 == 0 || l3 == 1)
                q_wire->attributes["\\init"] = RTLIL::Const(l3);
            else if (l3 == l1) {
                //q_wire->attributes["\\init"] = RTLIL::Const(RTLIL::State::Sx);
            }
            else
                log_error("Line %u has invalid reset literal for latch!\n", line_count);
        }
        else {
            // AIGER latches are assumed to be initialized to zero
            q_wire->attributes["\\init"] = RTLIL::Const(0);
        }
        latches.push_back(q_wire);
    }

    // Parse outputs
    std::vector<RTLIL::Wire*> outputs;
    for (unsigned i = 0; i < O; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an output!\n", line_count);

        log_debug("%d is an output\n", l1);
        RTLIL::Wire *wire = createWireIfNotExists(module, l1);
        wire->port_output = true;
        outputs.push_back(wire);
    }
    std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse bad state properties
    for (unsigned i = 0; i < B; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse invariant constraints
    for (unsigned i = 0; i < C; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse justice properties
    for (unsigned i = 0; i < J; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse fairness constraints
    for (unsigned i = 0; i < F; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // Parse AND
    for (unsigned i = 0; i < A; ++i, ++line_count) {
        if (!(f >> l1 >> l2 >> l3))
            log_error("Line %u cannot be interpreted as an AND!\n", line_count);

        log_debug("%d %d %d is an AND\n", l1, l2, l3);
        log_assert(!(l1 & 1)); // TODO: Output of ANDs can't be inverted?
        RTLIL::Wire *o_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *i1_wire = createWireIfNotExists(module, l2);
        RTLIL::Wire *i2_wire = createWireIfNotExists(module, l3);

		RTLIL::Cell *and_cell = module->addCell(NEW_ID, "$_AND_");
		and_cell->setPort("\\A", i1_wire);
		and_cell->setPort("\\B", i2_wire);
		and_cell->setPort("\\Y", o_wire);
    }
    std::getline(f, line); // Ignore up to start of next line

    std::string s;
    for (int c = f.peek(); c != EOF; c = f.peek(), ++line_count) {
        if (c == 'i' || c == 'l' || c == 'o') {
            f.ignore(1);
            if (!(f >> l1 >> s))
                log_error("Line %u cannot be interpreted as a symbol entry!\n", line_count);

            if ((c == 'i' && l1 > inputs.size()) || (c == 'l' && l1 > latches.size()) || (c == 'o' && l1 > outputs.size()))
                log_error("Line %u has invalid symbol position!\n", line_count);

            RTLIL::Wire* wire;
            if (c == 'i') wire = inputs[l1];
            else if (c == 'l') wire = latches[l1];
            else if (c == 'o') wire = outputs[l1];
            else log_abort();

            module->rename(wire, stringf("\\%s", s.c_str()));
        }
        else if (c == 'b' || c == 'j' || c == 'f') {
            // TODO
        }
        else if (c == 'c') {
            f.ignore(1);
            if (f.peek() == '\n')
                break;
            // Else constraint (TODO)
            break;
        }
        else
            log_error("Line %u: cannot interpret first character '%c'!\n", line_count, c);
        std::getline(f, line); // Ignore up to start of next line
    }
}

static unsigned parse_next_delta_literal(std::istream &f, unsigned ref)
{
    unsigned x = 0, i = 0;
    unsigned char ch;
    while ((ch = f.get()) & 0x80)
        x |= (ch & 0x7f) << (7 * i++);
    return ref - (x | (ch << (7 * i)));
}

static void parse_aiger_binary(RTLIL::Module *module, std::istream &f, std::string clk_name)
{
    unsigned M, I, L, O, A;
    unsigned B=0, C=0, J=0, F=0; // Optional in AIGER 1.9
    parse_aiger_header(f, M, I, L, O, A, B, C, J, F);

    unsigned line_count = 1;
    unsigned l1, l2, l3;
    std::string line;

    // Parse inputs
    std::vector<RTLIL::Wire*> inputs;
    for (unsigned i = 1; i <= I; ++i) {
        RTLIL::Wire *wire = createWireIfNotExists(module, i << 1);
        wire->port_input = true;
        inputs.push_back(wire);
    }

    // Parse latches
    std::vector<RTLIL::Wire*> latches;
    RTLIL::Wire *clk_wire = nullptr;
    if (L > 0) {
        RTLIL::IdString clk_id = RTLIL::escape_id(clk_name.c_str());
        clk_wire = module->wire(clk_id);
        log_assert(!clk_wire);
        log_debug("Creating %s\n", clk_id.c_str());
        clk_wire = module->addWire(clk_id);
        clk_wire->port_input = true;
    }
    l1 = (I+1) * 2;
    for (unsigned i = 0; i < L; ++i, ++line_count, l1 += 2) {
        if (!(f >> l2))
            log_error("Line %u cannot be interpreted as a latch!\n", line_count);
        log_debug("%d %d is a latch\n", l1, l2);
        RTLIL::Wire *q_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *d_wire = createWireIfNotExists(module, l2);

        module->addDff(NEW_ID, clk_wire, d_wire, q_wire);

        // Reset logic is optional in AIGER 1.9
        if (f.peek() == ' ') {
            if (!(f >> l3))
                log_error("Line %u cannot be interpreted as a latch!\n", line_count);

            if (l3 == 0 || l3 == 1)
                q_wire->attributes["\\init"] = RTLIL::Const(l3);
            else if (l3 == l1) {
                //q_wire->attributes["\\init"] = RTLIL::Const(RTLIL::State::Sx);
            }
            else
                log_error("Line %u has invalid reset literal for latch!\n", line_count);
        }
        else {
            // AIGER latches are assumed to be initialized to zero
            q_wire->attributes["\\init"] = RTLIL::Const(0);
        }
        latches.push_back(q_wire);
    }

    // Parse outputs
    std::vector<RTLIL::Wire*> outputs;
    for (unsigned i = 0; i < O; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an output!\n", line_count);

        log_debug("%d is an output\n", l1);
        RTLIL::Wire *wire = createWireIfNotExists(module, l1);
        wire->port_output = true;
        outputs.push_back(wire);
    }
    std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse bad state properties
    for (unsigned i = 0; i < B; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse invariant constraints
    for (unsigned i = 0; i < C; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse justice properties
    for (unsigned i = 0; i < J; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse fairness constraints
    for (unsigned i = 0; i < F; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // Parse AND
    l1 = (I+L+1) << 1;
    for (unsigned i = 0; i < A; ++i, ++line_count, l1 += 2) {
        l2 = parse_next_delta_literal(f, l1);
        l3 = parse_next_delta_literal(f, l2);

        log_debug("%d %d %d is an AND\n", l1, l2, l3);
        log_assert(!(l1 & 1)); // TODO: Output of ANDs can't be inverted?
        RTLIL::Wire *o_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *i1_wire = createWireIfNotExists(module, l2);
        RTLIL::Wire *i2_wire = createWireIfNotExists(module, l3);

		RTLIL::Cell *and_cell = module->addCell(NEW_ID, "$_AND_");
		and_cell->setPort("\\A", i1_wire);
		and_cell->setPort("\\B", i2_wire);
		and_cell->setPort("\\Y", o_wire);
    }
    std::getline(f, line); // Ignore up to start of next line

}

struct AigerFrontend : public Frontend {
	AigerFrontend() : Frontend("aiger", "read AIGER file") { }
	void help() YS_OVERRIDE
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    read_aiger [options] [filename]\n");
		log("\n");
		log("Load modules from an AIGER file into the current design.\n");
		log("\n");
	}
	void execute(std::istream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) YS_OVERRIDE
	{
		log_header(design, "Executing AIGER frontend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			break;
		}
		extra_args(f, filename, args, argidx);

		parse_aiger(design, *f);
	}
} AigerFrontend;

YOSYS_NAMESPACE_END
