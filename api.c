#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "database.h"
#include "registry.h"
#include "kernel.h"

#include "api.h"
#include "output.h"
#include "narrative.h"

/*---------------------------------------------------------------------------
	cn_entity
---------------------------------------------------------------------------*/
Entity *
cn_entity( char *name )
{
	registryEntry *entry = lookupByName( CN.registry, name );
	return ( entry == NULL ) ? NULL : (Entity *) entry->value;
}

/*---------------------------------------------------------------------------
	cn_name
---------------------------------------------------------------------------*/
char *
cn_name( Entity *e )
{
	return (char *) cn_va_get_value( "name", e );
}

/*---------------------------------------------------------------------------
	cn_new
---------------------------------------------------------------------------*/
Entity *
cn_new( char *name )
{
	Entity *e = newEntity( NULL, NULL, NULL );
	cn_va_set_value( "name", e, name );
	registerByName( &CN.registry, name, e );
	addItem( &CN.DB, e );
	addItem( &CN.context->frame.log.entities.instantiated, e );
	return e;
}

/*---------------------------------------------------------------------------
	cn_free
---------------------------------------------------------------------------*/
void
cn_free( Entity *e )
{
	void *name;

	// free entity's name, hcn and url strings

	name = cn_va_get_value( "name", e );
	free( name );
	name = cn_va_get_value( "hcn", e );
	free( name );
	name = cn_va_get_value( "url", e );
	free( name );

	// deregister entity from all its narratives

	Registry narratives = cn_va_get_value( "narratives", e );
	for ( registryEntry *i = narratives; i!=NULL; i=i->next )
	{
		Narrative *n = (Narrative *) i->value;
		removeFromNarrative( n, e );
	}
	freeRegistry( &narratives );

	// remove entity from name registry

	deregisterByValue( &CN.registry, e );

	// close all value accounts associated with this entity

	for ( registryEntry *i = CN.VB; i!=NULL; i=i->next ) {
		deregisterByAddress( (Registry *) &i->value, e );
	}

	removeItem( &CN.DB, e );
	freeEntity( e );
}

/*---------------------------------------------------------------------------
	cn_va_set_value
---------------------------------------------------------------------------*/
registryEntry *
cn_va_set_value( char *va_name, Entity *e, void *value )
{
	if ( value == NULL ) return NULL;
	registryEntry *entry = lookupByName( CN.VB, va_name );
	if ( entry == NULL ) return NULL;
	Registry *va = (Registry *) &entry->value;
	entry = lookupByAddress( *va, e );
	if ( entry == NULL ) {
		return registerByAddress( va, e, value );
	} else if ( !strcmp( va_name, "narratives" ) ) {
		// deregister entity from all previous narratives
		for (registryEntry *i = (Registry) entry->value; i!=NULL; i=i->next )
		{
			Narrative *n = (Narrative *) i->value;
			removeFromNarrative( n, e );
		}
		// reset value account
		freeRegistry((Registry *) &entry->value );
	} else {
		free( entry->value );
	}
	entry->value = value;
	return entry;
}

/*---------------------------------------------------------------------------
	cn_va_get_value
---------------------------------------------------------------------------*/
void *
cn_va_get_value( char *va_name, Entity *e )
{
	registryEntry *va = lookupByName( CN.VB, va_name );
	if ( va == NULL ) return NULL;
	registryEntry *entry = lookupByAddress((Registry) va->value, e );
	if ( entry == NULL ) return NULL;
	return entry->value;

}

/*---------------------------------------------------------------------------
	cn_instance
---------------------------------------------------------------------------*/
Entity *
cn_instance( Entity *source, Entity *medium, Entity *target )
{
	Entity *sub[3];
	sub[0] = source;
	sub[1] = medium;
	sub[2] = target;

	for ( int i=0; i<3; i++ )
	if ( sub[i] != NULL ) {
		for ( listItem *j = sub[i]->as_sub[i]; j!=NULL; j=j->next )
		{
			Entity *e = (Entity *) j->ptr;	
			int k = (i+1)%3;
			if ( e->sub[k] != sub[k] )
				continue;
			k = (k+1)%3;
			if ( e->sub[k] != sub[k] )
				continue;
			return e;
		}
		return NULL;
	}

	return NULL;
}

/*---------------------------------------------------------------------------
	cn_is_active
---------------------------------------------------------------------------*/
int
cn_is_active( Entity *e )
{
	return ( e->state == 1 ) ? 1 : 0;
}

/*---------------------------------------------------------------------------
	cn_instantiate
---------------------------------------------------------------------------*/
Entity *
cn_instantiate( Entity *source, Entity *medium, Entity *target )
{
	Entity *e = newEntity( source, medium, target );
	addItem( &CN.DB, e );
	addItem( &CN.context->frame.log.entities.instantiated, e );
	return e;
}

/*---------------------------------------------------------------------------
	cn_release
---------------------------------------------------------------------------*/
void
cn_release( Entity *e )
{
	cn_free( e );
#if 0
	addItem( &CN.context->frame.log.entities.released, full_expression( e ) );
#endif
}

/*---------------------------------------------------------------------------
	cn_activate
---------------------------------------------------------------------------*/
int
cn_activate( Entity *e )
{
	if ( !cn_is_active( e ) ) {
		e->state = 1;
		addItem( &CN.context->frame.log.entities.activated, e );
		return 1;
	}
	else return 0;
}

/*---------------------------------------------------------------------------
	cn_deactivate
---------------------------------------------------------------------------*/
int
cn_deactivate( Entity *e )
{
	if ( cn_is_active( e ) ) {
		e->state = 0;
		addItem( &CN.context->frame.log.entities.deactivated, e );
		return 1;
	}
	else return 0;
}

/*---------------------------------------------------------------------------
	cn_instantiate_narrative
---------------------------------------------------------------------------*/
int
cn_instantiate_narrative( Entity *e, Narrative *narrative )
{
	// does e already have a narrative with the same name?
	Narrative *n = lookupNarrative( e, narrative->name );
	if ( n != NULL ) {
		if ( e == CN.nil ) {
			fprintf( stderr, "consensus> Warning: narrative '%s()' already exists - cannot instantiate\n", narrative->name );
		} else {
			fprintf( stderr, "consensus> Warning: narrative %%'" ); output_name( e, NULL, 0 );
			fprintf( stderr, ".%s()' already exists - cannot instantiate\n", narrative->name );
		}
		return 0;
	}
	addNarrative( e, narrative->name, narrative );

	// log narrative instantiation event
	Registry *log = &CN.context->frame.log.narratives.instantiated;
	if ( *log == NULL ) {
		CN.context->frame.log.narratives.instantiated = newRegistryItem( narrative, newItem( e ) );
	} else {
		registryEntry *entry = lookupByAddress( *log, narrative );
		if ( entry == NULL ) {
			registerByAddress( log, narrative, newItem(e) );
		} else {
			addIfNotThere((listItem **) &entry->value, e );
		}
	}
	return 1;
}

/*---------------------------------------------------------------------------
	cn_activate_narrative
---------------------------------------------------------------------------*/
int
cn_activate_narrative( Entity *e, char *name )
{
	Narrative *n = lookupNarrative( e, name );
	if ( n == NULL ) return 0;

	// check if the requested narrative instance already exists
	if ( lookupByAddress( n->instances, e ) != NULL ) return 0;

	// log narrative activation event - the actual activation is performed in systemFrame
	Registry *log = &CN.context->frame.log.narratives.activate;
	if ( *log == NULL ) {
		CN.context->frame.log.narratives.activate = newRegistryItem( n, newItem( e ) );
	} else {
		registryEntry *entry = lookupByAddress( *log, n );
		if ( entry == NULL ) {
			registerByAddress( log, n, newItem(e) );
		} else {
			addIfNotThere((listItem **) &entry->value, e );
		}
	}
	return 1;
}