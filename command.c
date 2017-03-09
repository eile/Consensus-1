#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "database.h"
#include "registry.h"
#include "kernel.h"
#include "string_util.h"

#include "api.h"
#include "command.h"
#include "input.h"
#include "output.h"
#include "expression.h"
#include "narrative.h"
#include "variables.h"
#include "value.h"

// #define DEBUG

/*---------------------------------------------------------------------------
	execution engine
---------------------------------------------------------------------------*/
#define	command_do_( a, s ) \
	event = command_execute( a, &state, event, s, context );

#define BRKOUT if ( context->narrative.mode.one ) command_do_( nothing, "" ) else
#define BRKERR if ( context->narrative.mode.one ) command_do_( error, "" ) else

static int
command_execute( _action action, char **state, int event, char *next_state, _context *context )
{
	int retval;

	if ( action == push )
	{
#ifdef DEBUG
		fprintf( stderr, "debug> parser: command: pushing from level %d\n", context->control.level );
#endif
		event = push( *state, event, &next_state, context );
		*state = base;
	}
	else if ( !strcmp( next_state, pop_state ) )
	{
		event = action( *state, event, &next_state, context );
		*state = next_state;
	}
	else
	{
		event = action( *state, event, &next_state, context );
		if ( strcmp( next_state, same ) )
			*state = next_state;
	}
	return event;
}

/*---------------------------------------------------------------------------
	expression actions
---------------------------------------------------------------------------*/
static int
set_results_to_nil( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, 0, ExecutionMode ) )
		return 0;

	freeListItem( &context->expression.results );
	context->expression.results = newItem( CN.nil );
	return 0;
}

static int
set_expression_mode( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, InstructionMode, ExecutionMode ) )
		return 0;

	switch ( event ) {
	case '!': context->expression.mode = InstantiateMode; break;
	case '~': context->expression.mode = ReleaseMode; break;
	case '*': context->expression.mode = ActivateMode; break;
	case '_': context->expression.mode = DeactivateMode; break;
	}
	return 0;
}

static int
command_expression( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, 0, ExecutionMode ) )
		return 0;

	int retval = expression_solve( context->expression.ptr, 3, context );
	if ( retval == -1 ) return log_error( context, event, NULL );

	switch ( context->expression.mode ) {
	case InstantiateMode:
		// already done during expression_solve
		break;
	case ReleaseMode:
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next ) {
			Entity *e = (Entity *) i->ptr;
			cn_release( e );
		}
		break;
	case ActivateMode:
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next ) {
			Entity *e = (Entity *) i->ptr;
			cn_activate( e );
		}
		break;
	case DeactivateMode:
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next ) {
			Entity *e = (Entity *) i->ptr;
			cn_deactivate( e );
		}
		break;
	default:
		break;
	}
	return event;
}

static int
evaluate_expression( char *state, int event, char **next_state, _context *context )
{
	if ( context_check( FreezeMode, 0, 0 ) ) {
		command_do_( flush_input, same );
		return event;
	}

	bgn_
	in_( ">: %[" )	context->error.flush_output = 1;
	end

	// the mode may be needed by command_narrative
	int restore_mode = context->expression.mode;

	context->expression.mode = EvaluateMode;
	command_do_( parse_expression, same );
	if ( event > 0 ) {
		int retval = expression_solve( context->expression.ptr, 3, context );
		if ( retval < 0 ) event = retval;
	}

	context->expression.mode = restore_mode;
	return event;
}

int
read_expression( char *state, int event, char **next_state, _context *context )
{
	if ( context_check( FreezeMode, 0, 0 ) ) {
		command_do_( flush_input, same );
		return event;
	}

	// the mode may be needed by command_expression
	int restore_mode = context->expression.mode;

	context->expression.mode = ReadMode;
	command_do_( parse_expression, same );

	context->expression.mode = restore_mode;
	return event;
}

/*---------------------------------------------------------------------------
	narrative actions
---------------------------------------------------------------------------*/
static int
overwrite_narrative( Entity *e, _context *context )
{
	char *name = context->identifier.id[ 1 ].ptr;

	// does e already have a narrative with the same name?
	Narrative *n = lookupNarrative( e, name );
	if ( n == NULL ) return 1;

	// if yes, then is that narrative already active?
	else if ( lookupByAddress( n->instances, e ) != NULL )
	{
		if ( e == CN.nil ) {
			fprintf( stderr, "consensus> Warning: narrative '%s()' is already active - cannot overwrite\n", name );
		} else {
			fprintf( stderr, "consensus> Warning: narrative %%'" ); output_name( e, NULL, 0 );
			fprintf( stderr, ".%s()' is already active - cannot overwrite\n", name );
		}
		return 0;
	}
	else	// let's talk...
	{
		int overwrite = 0;
		if ( e == CN.nil ) {
			fprintf( stderr, "consensus> Warning: narrative '%s()' already exists. ", name );
		} else {
			fprintf( stderr, "consensus> Warning: narrative %%'" ); output_name( e, NULL, 0 );
			fprintf( stderr, ".%s()' already exists. ", name );
		}
		do {
			fprintf( stderr, "Overwrite? (y/n)_ " );
			overwrite = getchar();
			switch ( overwrite ) {
			case 'y':
			case 'n':
				if ( getchar() == '\n' )
					break;
			default:
				overwrite = 0;
				while ( getchar() != '\n' );
				break;
			}
		}
		while ( !overwrite );
		if ( overwrite == 'n' )
			return 0;

		removeNarrative( n, e );
	}
	return 1;
}

static int
command_narrative( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, 0, ExecutionMode ) )
		return 0;

	switch ( context->expression.mode ) {
	case InstantiateMode:
		; listItem *last_i = NULL, *next_i;
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next ) {
			next_i = i->next;
			if ( !overwrite_narrative(( Entity *) i->ptr, context ) ) {
				clipListItem( &context->expression.results, i, last_i, next_i );
			} else last_i = i;
		}
		read_narrative( state, event, next_state, context );
		Narrative *narrative = context->narrative.current;
		if ( narrative == NULL ) {
			return log_error( context, 0, "Narrative empty - not instantiated" );
		}
		int do_register = 0;
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next )
		{
			Entity *e = (Entity *) i->ptr;
			if ( cn_instantiate_narrative( e, narrative ) )
				do_register = 1;
		}
		if ( do_register ) {
			registerNarrative( narrative, context );
			fprintf( stderr, "consensus> narrative instantiated: %s()\n", narrative->name );
		} else {
			freeNarrative( narrative );
			fprintf( stderr, "consensus> Warning: no target entity - narrative not instantiated\n" );
		}
		context->narrative.current = NULL;
		break;

	case ActivateMode:
		; char *name = context->identifier.id[ 1 ].ptr;
		for ( listItem *i = context->expression.results; i!=NULL; i=i->next )
		{
			Entity *e = (Entity *) i->ptr;
			cn_activate_narrative( e, name );
		}
		event = 0;
		break;

	default:
		// only supports these modes for now...
		event = 0;
		break;
	}
	return 0;
}

static int
exit_narrative( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, InstructionMode, ExecutionMode ) )
		return 0;

	if ( context->narrative.current == NULL ) {
		return log_error( context, event, "'exit' command only supported in narrative mode" );
	}
	else if ( context->narrative.mode.block && ( context->control.mode == InstructionMode ) ) {
		return log_error( context, event, "'exit' is not a supported instruction - use 'do exit' action instead" );
	}
	else {
		context->narrative.current->deactivate = 1;
	}
	return event;
}

/*---------------------------------------------------------------------------
	va actions
---------------------------------------------------------------------------*/
static int
read_va( char *state, int event, char **next_state, _context *context )
{
	event = 0;	// we know it is '$'
	state = base;
	do {
	event = input( state, event, &same, context );
#ifdef DEBUG
	fprintf( stderr, "debug> read_va: in \"%s\", on '%c'\n", state, event );
#endif
	bgn_
	in_( base ) bgn_
		on_( '(' )	command_do_( nop, "(" )
		on_other	command_do_( error, "" )
		end
		in_( "(" ) bgn_
			on_( ' ' )	command_do_( nop, same )
			on_( '\t' )	command_do_( nop, same )
			on_other	command_do_( read_va_identifier, "(_" )
			end
			in_( "(_" ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( ')' )	command_do_( nop, "" )
				on_other	command_do_( error, "" )
				end
	end
	}
	while ( strcmp( state, "" ) );

	return event;
}

/*---------------------------------------------------------------------------
	condition actions
---------------------------------------------------------------------------*/
static int
set_condition_to_contrary( char *state, int event, char **next_state, _context *context )
{
	if ( !context_check( 0, 0, ExecutionMode ) )
		return 0;

	context->control.contrary = ~context->control.contrary;
	return 0;
}

static int
push_condition( int condition, char *state, int event, char **next_state, _context *context )
{
	command_do_( push, *next_state );
	*next_state = base;

	switch ( context->control.mode ) {
	case FreezeMode:
		break;
	case InstructionMode:
		; StackVA *stack = (StackVA *) context->control.stack->ptr;
		stack->condition = ConditionActive;
		break;
	case ExecutionMode:
		stack = (StackVA *) context->control.stack->ptr;
		stack->condition = condition ?
			( context->control.contrary ? ConditionPassive : ConditionActive ) :
			( context->control.contrary ? ConditionActive : ConditionPassive );
		if ( stack->condition == ConditionPassive )
			set_control_mode( FreezeMode, event, context );
	}
	context->control.contrary = 0;
	return 0;
}

static int
push_condition_from_variable( char *state, int event, char **next_state, _context *context )
{
	int condition = ( lookupVariable( context, context->identifier.id[ 0 ].ptr ) != NULL );
	return push_condition( condition, state, event, next_state, context );
}

static int
push_condition_from_expression( char *state, int event, char **next_state, _context *context )
{
	int condition = ( context->expression.results != NULL );
	return push_condition( condition, state, event, next_state, context );
}

static int
flip_condition( char *state, int event, char **next_state, _context *context )
{
	StackVA *stack = (StackVA *) context->control.stack->ptr;
	switch ( context->control.mode ) {
	case FreezeMode:
		if ( context->control.level == context->freeze.level )
		{
			stack->condition = ConditionActive;
			set_control_mode( ExecutionMode, event, context );
		}
		break;
	case InstructionMode:
		switch ( stack->condition ) {
			case ConditionActive:
			case ConditionPassive:
				break;
			case ConditionNone:
				return log_error( context, event, "not in conditional execution mode" );
		}
		break;
	case ExecutionMode:
		switch ( stack->condition ) {
			case ConditionActive:
				stack->condition = ConditionPassive;
				break;
			case ConditionPassive:
				stack->condition = ConditionActive;
				break;
			case ConditionNone:
				return log_error( context, event, "not in conditional execution mode" );
		}
		if ( stack->condition == ConditionPassive )
			set_control_mode( FreezeMode, event, context );
	}
	return 0;
}

/*---------------------------------------------------------------------------
	loop actions
---------------------------------------------------------------------------*/
static int
push_loop( char *state, int event, char **next_state, _context *context )
{
	if (( context->control.mode == ExecutionMode ) &&
	    ( context->input.stack == NULL ) && ( context->expression.results == NULL ))
	{
		return 0;	// do nothing in this case
	}

	command_do_( push, *next_state );
	*next_state = base;

	if ( !context_check( 0, 0, ExecutionMode ) )
		return 0;

	StackVA *stack = (StackVA *) context->control.stack->ptr;
	if ( context->input.stack == NULL ) {
		// here we are reading instructions from stdin
		set_control_mode( InstructionMode, event, context );
	}
	else {
		StreamVA *input = (StreamVA *) context->input.stack->ptr;
		if ( input->mode.block ) {
			// here we are executing a loop within a loop
			stack->loop.begin = context->control.execute->next;
		}
		else {
			// here we may be reading from a script
			set_control_mode( InstructionMode, event, context );
		}
	}
	if ( context->expression.results == NULL ) {
		set_control_mode( FreezeMode, event, context );
	}
	else {
		stack->loop.index = context->expression.results;
		context->expression.results = NULL;
		assign_variator_variable( stack->loop.index->ptr, context );
	}
	return 0;
}

/*---------------------------------------------------------------------------
	push_input_pipe, push_input_hcn
---------------------------------------------------------------------------*/
static int
command_push_input( InputType type, int event, _context *context )
{
	switch ( context->control.mode ) {
	case FreezeMode:
		break;
	case InstructionMode:
		// sets execution flag so that the last instruction is parsed again,
		// but this time in execution mode

		context->control.mode = ExecutionMode;
		context->control.execute = context->record.instructions;
		context->record.instructions = context->record.instructions->next;

		push_input( NULL, context->control.execute->ptr, EscapeStringInput, context );
		break;
	case ExecutionMode:
		if ( context->identifier.id[ 0 ].type != StringIdentifier ) {
			return log_error( context, event, "expected argument in \"quotes\"" );
		}
		char *identifier;
		if ( type == PipeInput ) {
			identifier = string_extract( context->identifier.id[ 0 ].ptr );
		} else if ( context->expression.results != NULL ) {
			Entity *entity = (Entity *) context->expression.results->ptr;
			identifier = (char *) cn_va_get_value( "hcn", entity );
		}
		push_input( identifier, NULL, type, context );
		break;
	}
	return 0;
}

int
push_input_pipe( char *state, int event, char **next_state, _context *context )
{
	return command_push_input( PipeInput, event, context );
}

int
push_input_hcn( char *state, int event, char **next_state, _context *context )
{
	return command_push_input( HCNFileInput, event, context );
}

/*---------------------------------------------------------------------------
	command_pop
---------------------------------------------------------------------------*/
int
command_pop( char *state, int event, char **next_state, _context *context )
{
	StackVA *stack = (StackVA *) context->control.stack->ptr;
	int do_pop = 1;

	switch ( context->control.mode ) {
	case FreezeMode:
		break;
	case InstructionMode:
		if ( context->record.instructions == NULL ) {
			break;	// not recording - most likely replaying
		}
		if ( context->control.level != context->record.level ) {
			if ( !strcmp( state, "/." ) ) {
				return log_error( context, event, "'/.' only allowed to close instruction block..." );
			}
			break;
		}
		// popping from narrative's block mode
		// -----------------------------------
		if ( context->narrative.mode.block ) {
			if ( context->record.instructions->next == NULL ) {
				if ( !strcmp( state, "/." ) ) {
					fprintf( stderr, "consensus> Warning: no action specified - will be removed from narrative...\n" );
					freeInstructionBlock( context );
				}
				else {
					free ( context->record.instructions->ptr );
					context->record.instructions->ptr = strdup( "~." );
				}
			}
			if ( !strcmp( state, "/" ) ) {
				*next_state = "";
				do_pop = 0;
			}
			break;
		}
		// popping from loop instruction mode - launches execution
		// -------------------------------------------------------
		// check that we do actually have recorded instructions to execute besides '/'
		if ( context->record.instructions->next == NULL ) {
			freeListItem( &stack->loop.index );
			freeInstructionBlock( context );
		} else {
			reorderListItem( &context->record.instructions );
			stack->loop.begin = context->record.instructions;
			context->control.execute = context->record.instructions;
			push_input( NULL, context->control.execute->ptr, BlockStringInput, context );
			*next_state = base;
			do_pop = 0;
		}
		set_control_mode( ExecutionMode, event, context );
		break;
	case ExecutionMode:
		// popping during narrative block execution
		// ----------------------------------------
		if (( context->narrative.mode.block ) && ( context->control.level == context->narrative.level )) {
			if ( !strcmp( state, "/" ) ) {
				*next_state = "";
				do_pop = 0;
			}
			break;
		}
		// popping during loop execution
		// -----------------------------
		if ( stack->loop.begin == NULL ) {
			;	// not in a loop
		}
		else if ( stack->loop.index->next == NULL ) {
			freeListItem( &stack->loop.index );
			if ( context->control.level == context->record.level ) {
				pop_input( state, event, next_state, context );
				freeInstructionBlock( context );
			}
			else {
				context->control.execute = context->control.execute->next;
				StreamVA *input = context->input.stack->ptr;
				input->ptr.string = context->control.execute->ptr;
				input->position = NULL;
			}
		}
		else {
			listItem *next_i = stack->loop.index->next;
			freeItem( stack->loop.index );
			stack->loop.index = next_i;
			assign_variator_variable( next_i->ptr, context );
			context->control.execute = stack->loop.begin;
			StreamVA *input = context->input.stack->ptr;
			input->ptr.string = context->control.execute->ptr;
			input->position = NULL;
			*next_state = base;
			do_pop = 0;
		}
		break;
	}

	if ( do_pop )
	{
		command_do_( pop, pop_state );

		if ( context->control.mode == FreezeMode ) {
			*next_state = base;
		}
		else if ( context->narrative.mode.block && ( context->control.level < context->record.level ) )
		{
			// popping from narrative instruction block - return control to narrative
			*next_state = "";
		}
		else {
			*next_state = state;
		}
	}

#ifdef DEBUG
	fprintf( stderr, "debug> command_pop: to state=\"%s\"\n", state );
#endif
	return ( do_pop ? event : 0 );
}

/*---------------------------------------------------------------------------
	read_command
---------------------------------------------------------------------------*/
int
read_command( char *state, int event, char **next_state, _context *context )
{
	do {
	event = input( state, event, &same, context );

#ifdef DEBUG
	fprintf( stderr, "main: \"%s\", '%c'\n", state, event );
#endif

	bgn_
	on_( -1 )	BRKOUT command_do_( flush_input, base )
	in_( base ) bgn_
		on_( ' ' )	command_do_( nop, same )
		on_( '\t' )	command_do_( nop, same )
		on_( '\n' )	command_do_( nop, same )
		on_( ':' )	command_do_( nop, ":" )
		on_( '>' )	command_do_( nop, ">" )
		on_( '%' )	command_do_( nop, "%" )		// HCN MODE ONLY
		on_( '?' )	BRKERR command_do_( nop, "?" )
		on_( '/' )	BRKERR command_do_( nop, "/" )
		on_( '!' )	command_do_( nop, "!" )
		on_( '~' )	command_do_( nop, "~" )
		on_( 'e' )	command_do_( nop, "e" )
		on_other	command_do_( error, base )
		end
		in_( "!" ) bgn_
			on_( '!' )	command_do_( set_expression_mode, "!." )
			on_( '~' )	command_do_( set_expression_mode, "!." )
			on_( '*' )	command_do_( set_expression_mode, "!." )
			on_( '_' )	command_do_( set_expression_mode, "!." )
			on_other	command_do_( error, base )
			end
			in_( "!." ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( '%' )	command_do_( nop, "!. %" )
				on_other	command_do_( read_expression, "!. expression" )
				end
				in_( "!. expression" ) bgn_
					on_( ' ' )	BRKOUT command_do_( nop, same )
					on_( '\t' )	BRKOUT command_do_( nop, same )
					on_( '\n' )	BRKOUT command_do_( command_expression, base )
					on_( '(' )	command_do_( set_results_to_nil, "!. narrative(" )
					on_other	BRKOUT command_do_( error, base )
					end
				in_( "!. narrative(" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( ')' )	command_do_( nop, "!. narrative(_)" )
					on_other	command_do_( error, base )
					end
					in_( "!. narrative(_)" ) bgn_
						on_( ' ' )	BRKOUT command_do_( nop, same )
						on_( '\t' )	BRKOUT command_do_( nop, same )
						on_( '\n' )	BRKOUT command_do_( command_narrative, base )
						on_other	BRKOUT command_do_( error, base )
						end
				in_( "!. %" ) bgn_
					on_( '[' )	command_do_( nop, "!. %[" )
					on_other	command_do_( read_expression, "!. expression" )
					end
					in_( "!. %[" ) bgn_
						on_any	command_do_( evaluate_expression, "!. %[_" )
						end
						in_( "!. %[_" ) bgn_
							on_( ']' )	command_do_( nop, "!. %[_]" )
							on_other	command_do_( error, base )
							end
							in_( "!. %[_]" ) bgn_
								on_( '.' )	command_do_( nop, "!. %[_]." )
								on_other	command_do_( error, base )
								end
								in_( "!. %[_]." ) bgn_
									on_any	command_do_( read_argument, "!. %[_].arg" )
									end
									in_( "!. %[_].arg" ) bgn_
										on_( ' ' )	command_do_( nop, same )
										on_( '\t' )	command_do_( nop, same )
										on_( '(' )	command_do_( nop, "!. %[_].arg(" )
										on_other	command_do_( error, base )
										end
										in_( "!. %[_].arg(" ) bgn_
											on_( ' ' )	command_do_( nop, same )
											on_( '\t' )	command_do_( nop, same )
											on_( ')' )	command_do_( nop, "!. %[_].arg(_)" )
											on_other	command_do_( error, base )
											end
											in_( "!. %[_].arg(_)" ) bgn_
												on_( ' ' )	BRKOUT command_do_( nop, same )
												on_( '\t' )	BRKOUT command_do_( nop, same )
												on_( '\n' )	BRKOUT command_do_( command_narrative, base )
												on_other	BRKOUT command_do_( error, base )
												end
		in_( "~" ) bgn_
			on_( '.' )	command_do_( nop, "~." )
			on_other	command_do_( error, base )
			end
			in_( "~." ) bgn_
				on_( '\n' )	BRKOUT command_do_( nop, base )
				on_other	command_do_( error, base )
				end
		in_( "e" ) bgn_
			on_( 'x' )	command_do_( nop, "ex" )
			on_other	command_do_( error, base )
			end
			in_( "ex" ) bgn_
				on_( 'i' )	command_do_( nop, "exi" )
				on_other	command_do_( error, base )
				end
				in_( "exi" ) bgn_
					on_( 't' )	command_do_( nop, "exit" )
					on_other	command_do_( error, base )
					end
					in_( "exit" ) bgn_
						on_( '\n' )	BRKOUT command_do_( exit_narrative, base ) // ONLY IN NARRATIVE MODE
						on_other	command_do_( error, base )
						end

	in_( "%" ) bgn_
		on_( '?' )	BRKERR command_do_( output_variator_value, base )	// ONLY IN HCN MODE
		on_other	BRKERR command_do_( read_identifier, "%variable" )	// ONLY IN HCN MODE
		end
		in_( "%variable" ) bgn_
			on_any	command_do_( output_variable_value, base )
			end

	in_( "/" ) bgn_
		on_( '~' )	command_do_( nop, "/~" )
		on_( '\n' )	command_do_( command_pop, pop_state )
		on_( '.' )	command_do_( nop, "/." )
		on_other	command_do_( error, base )
		end
		in_( "/~" ) bgn_
			on_( '\n' )	command_do_( flip_condition, base )
			on_other	command_do_( error, base )
			end
		in_( "/." ) bgn_
			on_( '\n' )	command_do_( command_pop, pop_state )
			on_other	command_do_( error, base )
			end

	in_( ">" ) bgn_
		on_( ' ' )	command_do_( nop, same )
		on_( '\t' )	command_do_( nop, same )
		on_( ':' )	command_do_( nop, ">:" )
		on_other	command_do_( error, base )
		end
		in_( ">:" ) bgn_
			on_( '\n' )	BRKOUT command_do_( output_char, base )
			on_( '%' )	command_do_( nop, ">: %" )
			on_other	command_do_( output_char, same )
			end
			in_( ">: %" ) bgn_
				on_( '?' )	command_do_( output_variator_value, ">:" )
				on_( '.' )	command_do_( set_results_to_nil, ">: %[_]." )
				on_( '[' )	command_do_( nop, ">: %[" )
				on_( '\n' )	BRKOUT command_do_( output_mod, base )
				on_separator	command_do_( output_mod, ">:" )
				on_other	command_do_( read_identifier, ">: %variable" )
				end
				in_( ">: %variable" ) bgn_
					on_( '\n' )	BRKOUT command_do_( output_variable_value, base )
					on_other	command_do_( output_variable_value, ">:" )
					end
				in_( ">: %[" ) bgn_
					on_any	command_do_( evaluate_expression, ">: %[_" )
					end
					in_( ">: %[_" ) bgn_
						on_( ']' )	command_do_( nop, ">: %[_]")
						on_other	command_do_( error, base )
						end
						in_( ">: %[_]" ) bgn_
							on_( '.' )	command_do_( nop, ">: %[_]." )
							on_( '\n' )	BRKOUT command_do_( output_expression_results, base )
							on_other	command_do_( output_expression_results, ">:" )
							end
					in_( ">: %[_]." ) bgn_
						on_( '$' )	command_do_( read_va, ">: %[_].$" )
						on_( '%' )	command_do_( nop, ">: %[_].%" )
						on_other	command_do_( read_argument, ">: %[_].arg" )
						end
						in_( ">: %[_].$" ) bgn_
							on_( '\n' )	BRKOUT command_do_( output_va, base )
							on_other	command_do_( output_va, ">:" )
							end
						in_( ">: %[_].arg" ) bgn_
							on_( ' ' )	command_do_( nop, same )
							on_( '\t' )	command_do_( nop, same )
							on_( '(' )	command_do_( nop, ">: %[_].narrative(" )
							on_other	command_do_( error, base )
							end
							in_( ">: %[_].narrative(" ) bgn_
								on_( ' ' )	command_do_( nop, same )
								on_( '\t' )	command_do_( nop, same )
								on_( ')' )	command_do_( nop, ">: %[_].narrative(_)" )
								on_other	command_do_( error, base )
								end
								in_( ">: %[_].narrative(_)" ) bgn_
									on_( '\n' )	BRKOUT command_do_( output_narrative, base )
									on_other	command_do_( output_narrative, ">:" )
									end

	in_( "?" ) bgn_
		on_( ' ' )	command_do_( nop, same )
		on_( '\t' )	command_do_( nop, same )
		on_( '~' )	command_do_( nop, "?~" )
		on_( '\n' )	command_do_( error, base )
		on_( '%' )	command_do_( nop, "? %" )
		on_( ':' )	command_do_( nop, "? i:" )
		on_other	command_do_( read_identifier, "? identifier" )
		end
		in_( "?~" ) bgn_
			on_( ' ' )	command_do_( nop, same )
			on_( '\t' )	command_do_( nop, same )
			on_( '~' )	command_do_( nop, "?" )
			on_( '%' )	command_do_( nop, "?~ %" )
			on_other	command_do_( error, base )
			end
			in_( "?~ %" ) bgn_
				on_( '[' )	command_do_( evaluate_expression, "?~ %[_]" )
				on_other	command_do_( read_identifier, "?~ %variable" )
				end
				in_( "?~ %[_]" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( '\n' )	command_do_( set_condition_to_contrary, same )
							command_do_( push_condition_from_expression, base )
					on_other	command_do_( error, same )
					end
				in_( "?~ %variable" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( '\n' )	command_do_( set_condition_to_contrary, same )
							command_do_( push_condition_from_variable, base )
					on_other	command_do_( error, same )
					end
		in_( "? %" ) bgn_
			on_( '[' )	command_do_( evaluate_expression, "? %[_]" )
			on_other	command_do_( read_identifier, "? %variable" )
			end
			in_( "? %[_]" ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( '\n' )	command_do_( push_condition_from_expression, base )
				on_other	command_do_( error, base )
				end
			in_( "? %variable" ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( '\n' )	command_do_( push_condition_from_variable, base )
				on_other	command_do_( error, base )
				end

		in_( "? identifier" ) bgn_
			on_( ' ' )	command_do_( nop, same )
			on_( '\t' )	command_do_( nop, same )
			on_( '\n' )	command_do_( error, base )
			on_( ':' )	command_do_( nop, "? i:" )
			on_other	command_do_( error, base )
			end
			in_( "? i:" ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( '\n' )	command_do_( error, base )
				on_other	command_do_( evaluate_expression, "? i: expression" )
				end
				in_( "? i: expression" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( '\n' )	command_do_( push_loop, base )
					on_other	command_do_( error, base )
					end

	in_( ":" ) bgn_
		on_( ' ' )	command_do_( nop, same )
		on_( '\t' )	command_do_( nop, same )
		on_( '\n' )	command_do_( error, base )
		on_( '%' )	command_do_( nop, ": %" )
		on_( '<' )	command_do_( nop, ":<" )
		on_other	command_do_( read_identifier, ": identifier" )
		end
		in_( ": identifier" ) bgn_
			on_( ' ' )	command_do_( nop, same )
			on_( '\t' )	command_do_( nop, same )
			on_( '\n' )	command_do_( error, base )
			on_( ':' )	command_do_( nop, ": identifier :" )
			on_other	command_do_( error, base )
			end
			in_( ": identifier :" ) bgn_
				on_( ' ' )	command_do_( nop, same )
				on_( '\t' )	command_do_( nop, same )
				on_( '%' )	command_do_( nop, ": identifier : %" )
				on_( '\n' )	command_do_( error, base )
				on_( '!' )	command_do_( nop, ": identifier : !" )
				on_other	command_do_( read_expression, ": identifier : expression" )
				end
				in_( ": identifier : expression" ) bgn_
					on_( '(' )	command_do_( set_results_to_nil, ": identifier : narrative(" )
					on_( '\n' )	BRKOUT command_do_( assign_expression, base )
					on_other	command_do_( error, base )
					end
				in_( ": identifier : %" ) bgn_
					on_( '[' )	command_do_( nop, ": identifier : %[" )
					on_other	command_do_( read_expression, ": identifier : expression" )
					end
					in_( ": identifier : %[" ) bgn_
						on_any	command_do_( evaluate_expression, ": identifier : %[_" )
						end
						in_( ": identifier : %[_" ) bgn_
							on_( ' ' )	command_do_( nop, same )
							on_( '\t' )	command_do_( nop, same )
							on_( ']' )	command_do_( nop, ": identifier : %[_]" )
							on_other	command_do_( error, base )
							end
							in_( ": identifier : %[_]" ) bgn_
								on_( ' ' )	command_do_( nop, same )
								on_( '\t' )	command_do_( nop, same )
								on_( '\n' )	BRKOUT command_do_( assign_results, base )
								on_other	command_do_( error, base )
								end
				in_( ": identifier : !" ) bgn_
					on_( '!' )	command_do_( set_expression_mode, same )
							command_do_( read_expression, ": identifier : !." )
					on_( '~' )	command_do_( set_expression_mode, same )
							command_do_( read_expression, ": identifier : !." )
					on_( '*' )	command_do_( set_expression_mode, same )
							command_do_( read_expression, ": identifier : !." )
					on_( '_' )	command_do_( set_expression_mode, same )
							command_do_( read_expression, ": identifier : !." )
					on_other	command_do_( error, base )
					end
					in_( ": identifier : !." ) bgn_
						on_any	command_do_( command_expression, ": identifier : !. expression" )
						end
						in_( ": identifier : !. expression" ) bgn_
							on_( '(' )	command_do_( set_results_to_nil, ": identifier : !. narrative(" )
							on_( '\n' )	BRKOUT command_do_( assign_results, base )
							on_other	command_do_( error, base )
							end
						in_( ": identifier : !. narrative(" ) bgn_
							on_( ' ' )	command_do_( nop, same )
							on_( '\t' )	command_do_( nop, same )
							on_( ')' )	command_do_( nop, ": identifier : !. narrative(_)" )
							on_other	command_do_( error, base )
							end
							in_( ": identifier : !. narrative(_)" ) bgn_
								on_( ' ' )	command_do_( nop, same )
								on_( '\t' )	command_do_( nop, same )
								on_( '\n' )	BRKOUT {
										command_do_( command_narrative, same )
										command_do_( assign_narrative, base )
										}
								on_other	command_do_( error, base )
								end
				in_( ": identifier : narrative(" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( ')' )	command_do_( nop, ": identifier : narrative(_)" )
					on_other	command_do_( error, base )
					end
					in_( ": identifier : narrative(_)" ) bgn_
						on_( ' ' )	command_do_( nop, same )
						on_( '\t' )	command_do_( nop, same )
						on_( '\n' )	BRKOUT command_do_( assign_narrative, base )
						on_other	command_do_( error, base )
						end

		in_( ": %" ) bgn_
			on_( '.' )	command_do_( set_results_to_nil, ": %[_]." )
			on_( '[' )	command_do_( nop, ": %[" )
			on_other	command_do_( error, base )
			end
			in_( ": %[" ) bgn_
				on_any	command_do_( evaluate_expression, ": %[_" )
				end
				in_( ": %[_" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( ']' )	command_do_( nop, ": %[_]" )
					on_other	command_do_( error, base )
					end
					in_( ": %[_]" ) bgn_
						on_( ' ' )	command_do_( nop, same )
						on_( '\t' )	command_do_( nop, same )
						on_( '.' )	command_do_( nop, ": %[_]." )
						on_other	command_do_( error, base )
						end
			in_( ": %[_]." ) bgn_
				on_( '$' )	command_do_( read_va, ": %[_].$" )
				on_other	command_do_( error, base )
				end
				in_( ": %[_].$" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_( ':' )	command_do_( nop, ": %[_].$:" )
					on_other	command_do_( error, base )
					end
					in_( ": %[_].$:" ) bgn_
						on_( ' ' )	command_do_( nop, same )
						on_( '\t' )	command_do_( nop, same )
						on_( '%' )	command_do_( nop, ": %[_].$: %" )
						on_other	command_do_( read_argument, ": %[_].$: arg" )
						end
						in_( ": %[_].$: %" ) bgn_
							on_any	command_do_( read_argument, ": %[_].$: %arg" )
							end
							in_( ": %[_].$: %arg" ) bgn_
								on_( ' ' )	command_do_( nop, same )
								on_( '\t' )	command_do_( nop, same )
								on_( '\n' )	BRKOUT command_do_( set_va_from_variable, base )
								on_other	command_do_( error, base )
								end
						in_( ": %[_].$: arg" ) bgn_
							on_( ' ' )	command_do_( nop, same )
							on_( '\t' )	command_do_( nop, same )
							on_( '(' )	command_do_( nop, ": %[_].$: arg(" )
							on_( '\n' )	BRKOUT command_do_( set_va, base )
							on_other	command_do_( error, base )
							end
							in_( ": %[_].$: arg(" ) bgn_
								on_( ' ' )	command_do_( nop, same )
								on_( '\t' )	command_do_( nop, same )
								on_( ')' )	command_do_( nop, ": %[_].$: arg(_)" )
								on_other	command_do_( error, base )
								end
								in_( ": %[_].$: arg(_)" ) bgn_
									on_( ' ' )	command_do_( nop, same )
									on_( '\t' )	command_do_( nop, same )
									on_( '\n' )	BRKOUT command_do_( set_va, base )
									on_other	command_do_( error, base )
									end

		in_( ":<" ) bgn_
			on_( ' ' )	command_do_( nop, same )
			on_( '\t' )	command_do_( nop, same )
			on_( '%' )	command_do_( nop, ":< %" )
			on_other	command_do_( error, base )
			end
			in_( ":< %" ) bgn_
				on_( '(' )	command_do_( nop, ":< %(" )
				on_other	command_do_( error, base )
				end
				in_( ":< %(" ) bgn_
					on_( ' ' )	command_do_( nop, same )
					on_( '\t' )	command_do_( nop, same )
					on_other	command_do_( read_identifier, ":< %( string" )
					end
					in_( ":< %( string" ) bgn_
						on_( ' ' )	command_do_( nop, same )
						on_( '\t' )	command_do_( nop, same )
						on_( ')' )	command_do_( nop, ":< %( string )" )
						on_other	command_do_( error, base )
						end
						in_( ":< %( string )" ) bgn_
							on_( ' ' )	BRKOUT command_do_( nop, same )
							on_( '\t' )	BRKOUT command_do_( nop, same )
							on_( '\n' )	BRKOUT command_do_( push_input_pipe, base )
							on_other	command_do_( error, base )
							end
	end
	}
	while ( strcmp( state, "" ) );

	return event;
}
