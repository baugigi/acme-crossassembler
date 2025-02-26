// ACME - a crossassembler for producing 6502/65c02/65816/65ce02 code.
// Copyright (C) 1998-2024 Marco Baye
// Have a look at "acme.c" for further info
//
// symbol stuff
//
// 22 Nov 2007	"warn on indented labels" is now a CLI switch
// 25 Sep 2011	Fixed bug in !sl (colons in filename could be interpreted as EOS)
// 23 Nov 2014	Added label output in VICE format
#include "symbol.h"
#include <stdio.h>
#include <string.h>	// for memcpy()
#include "alu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "output.h"
#include "platform.h"
#include "section.h"
#include "tree.h"
#include "typesystem.h"


// variables
struct rwnode	*symbols_forest[256]	= { NULL };	// because of 8-bit hash - must be (at least partially) pre-defined so array will be zeroed!


// Dump symbol value and flags to dump file
static void dump_one_symbol(struct rwnode *node, FILE *fd)
{
	struct symbol	*symbol	= node->body;

	// if symbol is neither int nor float, skip
	if (symbol->object.type != &type_number)
		return;

	// CAUTION: if more types are added, check for NULL before using type pointer!

	// output name
	if (config.warn_on_type_mismatch
	&& symbol->object.u.number.addr_refs == 1)
		fprintf(fd, "!addr");
	fprintf(fd, "\t%s", node->id_string);
	switch (symbol->object.u.number.flags & NUMBER_FORCEBITS) {
	case NUMBER_FORCES_16:
		fprintf(fd, "+2\t= ");
		break;
	case NUMBER_FORCES_16 | NUMBER_FORCES_24:
		/*FALLTHROUGH*/
	case NUMBER_FORCES_24:
		fprintf(fd, "+3\t= ");
		break;
	default:
		fprintf(fd, "\t= ");
	}
	if (symbol->object.u.number.ntype == NUMTYPE_UNDEFINED)
		fprintf(fd, " ?");	// TODO - maybe write "UNDEFINED" instead? then the file could at least be parsed without errors
	else if (symbol->object.u.number.ntype == NUMTYPE_INT)
		fprintf(fd, "$%x", (unsigned) symbol->object.u.number.val.intval);
	else if (symbol->object.u.number.ntype == NUMTYPE_FLOAT)
		fprintf(fd, "%.30f", symbol->object.u.number.val.fpval);	//FIXME %g
	else
		BUG("IllegalNumberType4", symbol->object.u.number.ntype);
	if (symbol->object.u.number.flags & NUMBER_EVER_UNDEFINED)
		fprintf(fd, "\t; ?");	// TODO - write "forward" instead?
	if (!symbol->has_been_read)
		fprintf(fd, "\t; unused");
	fprintf(fd, "\n");
}


// output symbols in VICE format (example: "al C:09ae .nmi1")
static void dump_vice_address(struct rwnode *node, FILE *fd)
{
	struct symbol	*symbol	= node->body;

	// dump address symbols even if they are not used
	if ((symbol->object.type == &type_number)
	&& (symbol->object.u.number.ntype == NUMTYPE_INT)
	&& (symbol->object.u.number.addr_refs == 1))
		fprintf(fd, "al C:%04x .%s\n", (unsigned) symbol->object.u.number.val.intval, node->id_string);
}
static void dump_vice_usednonaddress(struct rwnode *node, FILE *fd)
{
	struct symbol	*symbol	= node->body;

	// dump non-addresses that are used
	if (symbol->has_been_read
	&& (symbol->object.type == &type_number)
	&& (symbol->object.u.number.ntype == NUMTYPE_INT)
	&& (symbol->object.u.number.addr_refs != 1))
		fprintf(fd, "al C:%04x .%s\n", (unsigned) symbol->object.u.number.val.intval, node->id_string);
}
static void dump_vice_unusednonaddress(struct rwnode *node, FILE *fd)
{
	struct symbol	*symbol	= node->body;

	// dump non-addresses that are unused
	if (!symbol->has_been_read
	&& (symbol->object.type == &type_number)
	&& (symbol->object.u.number.ntype == NUMTYPE_INT)
	&& (symbol->object.u.number.addr_refs != 1))
		fprintf(fd, "al C:%04x .%s\n", (unsigned) symbol->object.u.number.val.intval, node->id_string);
}


// search for symbol. if it does not exist, create with NULL object (CAUTION!).
// the symbol name must be held in GlobalDynaBuf.
struct symbol *symbol_find(scope_t scope)
{
	struct rwnode	*node;
	struct symbol	*symbol;
	boolean		node_created;

	node_created = tree_hard_scan(&node, symbols_forest, scope, TRUE);
	// if node has just been created, create symbol as well
	if (node_created) {
		// create new symbol structure
		symbol = safe_malloc(sizeof(*symbol));
		node->body = symbol;
		// finish empty symbol item
		symbol->object.type = NULL;	// no object yet (CAUTION!)
		symbol->has_been_read = FALSE;
		symbol->has_been_reported = FALSE;
		symbol->pseudopc = NULL;
		// we must set "definition" fields to dummy data, because the object
		// has been created, but not necessarily set to a defined value:
		symbol->definition.plat_filename = NULL;
		symbol->definition.line_number = 0;
		symbol->pass_number = pass.number;
	} else {
		symbol = node->body;
	}
	return symbol;	// now symbol->object.type can be tested to see if this was freshly created.
	// CAUTION: this only works if caller always sets a type pointer after checking! if NULL is kept, the struct still looks new later on...
}


// assign object to symbol. the function acts upon the symbol's flag bits and
// produces an error if needed.
// using "power" bits, caller can state which changes are ok.
// called by:
// implicit label definitions (including anons, backward anons have POWER_CHANGE_VALUE)
// explicit symbol assignments
// explicit symbol assignments via "!set" (has all powers)
// loop counter var init via "!for" (has POWER_CHANGE_VALUE and POWER_CHANGE_NUMTYPE)
//	CAUTION: actual incrementing of counter is then done directly without calls here!
void symbol_set_object(struct symbol *symbol, struct object *new_value, bits powers)
{
	boolean	redefined;

	if (symbol->object.type == NULL) {
		// symbol has no object assigned to it yet
		symbol->object = *new_value;	// copy whole struct including type
		// as long as the symbol has not been read, the force bits can
		// be changed, so the caller still has a chance to do that.
	} else {
		// symbol already has an object

		// compare types
		// if too different, needs power (or complains)
		if (symbol->object.type != new_value->type) {
			if (!(powers & POWER_CHANGE_OBJTYPE))
				throw_redef_error(exception_symbol_defined, &symbol->definition, "Previous definition.");
			// CAUTION: if line above triggers, we still go ahead and change type!
			// this is to keep "!for" working, where the counter var is accessed.
			symbol->object = *new_value;	// copy whole struct including type
			// clear flag so caller can adjust force bits:
			symbol->has_been_read = FALSE;	// it's basically a new symbol now
		} else {
			// symbol and new value have compatible types, so call handler:
			redefined = symbol->object.type->assign(&symbol->object, new_value, !!(powers & POWER_CHANGE_VALUE));
			if (redefined) {
				// do we accept re-definitions without "!set"?
				if (config.dialect >= V0_98__PATHS_AND_SYMBOLCHANGE) {
					// since version 0.98 new passes can assign new values:
					if (symbol->pass_number != pass.number) {
						++pass.counters.symbolchanges;
					} else {
						throw_redef_error(exception_symbol_defined, &symbol->definition, "Previous definition.");
					}
				} else {
					// older versions complained about _all_ re-definitions:
					throw_redef_error(exception_symbol_defined, &symbol->definition, "Previous definition.");
				}
			}
		}
	}
	// if symbol is an address, give it a pseudopc context:
	if ((symbol->object.type == &type_number)
	&& (symbol->object.u.number.addr_refs == 1)) {
		symbol->pseudopc = pseudopc_get_context();
	}
	// remember current location and pass number for "symbol twice" errors in future:
	input_get_location(&symbol->definition);
	symbol->pass_number = pass.number;
}


// set force bit of symbol. trying to change to a different one will raise error.
void symbol_set_force_bit(struct symbol *symbol, bits force_bit)
{
	if (!force_bit)
		BUG("ForceBitZero", 0);
	if (symbol->object.type == NULL)
		BUG("NullTypeObject", 0);

	if (symbol->object.type != &type_number) {
		throw_error("Force bits can only be given to numbers.");
		return;
	}

	// if change is ok, change
	if (!symbol->has_been_read) {
		symbol->object.u.number.flags &= ~NUMBER_FORCEBITS;
		symbol->object.u.number.flags |= force_bit;
		return;	// and be done with it
	}

	// it's too late to change, so check if the wanted bit is actually different
	if ((symbol->object.u.number.flags & NUMBER_FORCEBITS) != force_bit)
		throw_error("Too late for postfix.");
}


// create and return symbol for "-D" command line switch (with NULL type object, CAUTION!).
// name must be held in GlobalDynaBuf
extern struct symbol *symbol_for_cli_def(void)
{
	struct symbol	*symbol;

	symbol = symbol_find(SCOPE_GLOBAL);
	symbol->definition.plat_filename = "\"-D SYMBOL=VALUE\"";
	symbol->definition.line_number = 1;
	return symbol;
}
// set symbol to integer value, no questions asked (for "-D" switch)
// FIXME - remove and call int_create_byte instead?
void symbol_define_int(struct symbol *symbol, intval_t value)
{
	symbol->object.type = &type_number;
	symbol->object.u.number.ntype = NUMTYPE_INT;
	symbol->object.u.number.flags = 0;
	symbol->object.u.number.val.intval = value;
	symbol->object.u.number.addr_refs = 0;
}
// set symbol to string value, no questions asked (for "-D" switch)
// string value must be held in GlobalDynaBuf
void symbol_define_string(struct symbol *symbol)
{
	string_prepare_string(&symbol->object, GlobalDynaBuf->size);
	memcpy(symbol->object.u.string->payload, GLOBALDYNABUF_CURRENT, GlobalDynaBuf->size);
}


// dump global symbols to file
void symbols_list(FILE *fd)
{
	tree_dump_forest(symbols_forest, SCOPE_GLOBAL, dump_one_symbol, fd);
}


void symbols_vicelabels(FILE *fd)
{
	// FIXME - if type checking is enabled, maybe only output addresses?
	// the order of dumped labels is important because VICE will prefer later defined labels
	// dump unused labels
	tree_dump_forest(symbols_forest, SCOPE_GLOBAL, dump_vice_unusednonaddress, fd);
	fputc('\n', fd);
	// dump other used labels
	tree_dump_forest(symbols_forest, SCOPE_GLOBAL, dump_vice_usednonaddress, fd);
	fputc('\n', fd);
	// dump address symbols
	tree_dump_forest(symbols_forest, SCOPE_GLOBAL, dump_vice_address, fd);
	// TODO - add trace points and watch points with load/store/exec args!
}


// fix name of anonymous forward label (held in DynaBuf, NOT TERMINATED!) so it
// references the *next* anonymous forward label definition. The tricky bit is,
// each name length would need its own counter. But hey, ACME's real quick in
// finding symbols, so I'll just abuse the symbol system to store those counters.
// example:
//	forward anon name is "+++"
//	we look up that symbol's value in the current local scope -> $12
//	we attach hex digits to name -> "+++21"
//	that's the name of the symbol that _actually_ contains the address
// caller sets "increment" to TRUE for writing, FALSE for reading
void symbol_fix_forward_anon_name(boolean increment)
{
	struct symbol	*counter_symbol;
	unsigned long	number;

	// terminate name, find "counter" symbol and read value
	dynabuf_append(GlobalDynaBuf, '\0');
	counter_symbol = symbol_find(section_now->local_scope);
	if (counter_symbol->object.type == NULL) {
		// finish freshly created symbol item
		counter_symbol->object.type = &type_number;
		counter_symbol->object.u.number.ntype = NUMTYPE_INT;
		counter_symbol->object.u.number.flags = 0;
		counter_symbol->object.u.number.addr_refs = 0;
		counter_symbol->object.u.number.val.intval = 0;
	} else if (counter_symbol->object.type != &type_number) {
		// sanity check: it must be a number!
		BUG("ForwardAnonCounterNotInt", 0);
	}
	// make sure it gets reset to zero in each new pass
	if (counter_symbol->pass_number != pass.number) {
		counter_symbol->pass_number = pass.number;
		counter_symbol->object.u.number.val.intval = 0;
	}
	// now use value
	number = (unsigned long) counter_symbol->object.u.number.val.intval;
	if (increment)
		counter_symbol->object.u.number.val.intval++;
	// now append to the name to make it unique
	GlobalDynaBuf->size--;	// forget terminator, we want to append
	do {
		DYNABUF_APPEND(GlobalDynaBuf, 'a' + (number & 15));
		number >>= 4;
	} while (number);
	dynabuf_append(GlobalDynaBuf, '\0');
}
