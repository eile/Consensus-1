#ifndef INPUT_H
#define INPUT_H

/*---------------------------------------------------------------------------
	input actions	- public
---------------------------------------------------------------------------*/

_action	input;
_action	flush_input;
_action	read_identifier;
_action	read_argument;
_action	read_va_identifier;

/*---------------------------------------------------------------------------
	input utilities	- public
---------------------------------------------------------------------------*/

void	set_input_mode( RecordMode mode, int event, _context *context );
int	push_input( char *identifier, void *src, InputType type, _context *context );
void	set_input( listItem *src, _context *context );
int	pop_input( char *state, int event, char **next_state, _context *context );
void	freeInstructionBlock( _context *context );

#endif	// INPUT_H
