/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nautilus-directory-metafile.c: Nautilus directory model.
 
   Copyright (C) 2000, 2001 Eazel, Inc.
  
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
  
   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
  
   Authors: Darin Adler <darin@bentspoon.com>,
            Mike Engber <engber@eazel.com>
*/

#include <config.h>
#include "nautilus-directory-metafile.h"

#include "nautilus-directory-metafile-monitor.h"
#include "nautilus-directory-private.h"
#include "nautilus-metafile-factory.h"
#include "nautilus-metafile-server.h"
#include <bonobo-activation/bonobo-activation.h>
#include <eel/eel-debug.h>
#include <eel/eel-string.h>
#include <stdio.h>

static Nautilus_MetafileFactory factory = CORBA_OBJECT_NIL;
/* We disable the remote metafile factory, because there seems to be some
 * sort of races with bonobo activation that sometimes leads to crashes
 * like in bug #351713. This isn't really a problem anyway, since nautilus
 * is only one process these days, and we still use bonobo activation to
 * avoid starting multiple copies of it.
 */ 
static gboolean get_factory_from_oaf = FALSE;

void
nautilus_directory_use_self_contained_metafile_factory (void)
{
	g_return_if_fail (factory == CORBA_OBJECT_NIL);

	get_factory_from_oaf = FALSE;
}

static void
free_factory (void)
{
	CORBA_Object_release (factory, NULL);
	factory = CORBA_OBJECT_NIL;
}

static void
die_on_failed_activation (const char *server_name,
			  CORBA_Environment *ev)
{
	/* This isn't supposed to happen. So do some core-dumping
	 * action, and don't bother translating the error message.
	 */

	const char *details;
	Bonobo_GeneralError *general_error;

	switch (ev->_major) {
	case CORBA_NO_EXCEPTION:
		details = "got NIL but no exception";
		break;

	case CORBA_SYSTEM_EXCEPTION:
	case CORBA_USER_EXCEPTION:
		details = CORBA_exception_id (ev);
		if (strcmp (details, "IDL:Bonobo/GeneralError:1.0") == 0) {
			general_error = CORBA_exception_value (ev);
			details = general_error->description;
		}
		break;

	default:
		details = "got bad exception";
		break;
	}

	g_error ("Failed to activate the server %s; this may indicate a broken\n"
		 "Nautilus or Bonobo installation, or may reflect a bug in something,\n"
		 "or may mean that your PATH or LD_LIBRARY_PATH or the like is\n"
		 "incorrect. Nautilus will dump core and exit.\n"
		 "Details: '%s'", server_name, details);
}

static Nautilus_MetafileFactory
get_factory (void)
{
	CORBA_Environment ev;
	
	if (factory == CORBA_OBJECT_NIL) {
		CORBA_exception_init (&ev);

		if (get_factory_from_oaf) {
			factory = bonobo_activation_activate_from_id (METAFILE_FACTORY_IID, 0, NULL, &ev);
			if (ev._major != CORBA_NO_EXCEPTION || factory == CORBA_OBJECT_NIL) {
				die_on_failed_activation ("Nautilus_MetafileFactory", &ev);
			}
		} else {
			factory = CORBA_Object_duplicate
				(BONOBO_OBJREF (nautilus_metafile_factory_get_instance ()), &ev);
		}

		CORBA_exception_free (&ev);

		eel_debug_call_at_shutdown (free_factory);
	}

	return factory;
}

static Nautilus_Metafile
open_metafile (const char *uri, gboolean make_errors_fatal)
{
	Nautilus_Metafile metafile;
	CORBA_Environment ev;

	CORBA_exception_init (&ev);
	
	metafile = Nautilus_MetafileFactory_open (get_factory (), uri, &ev);
	
	if (ev._major != CORBA_NO_EXCEPTION) {
		metafile = CORBA_OBJECT_NIL;
		if (make_errors_fatal) {
			g_error ("%s: CORBA error opening MetafileFactory: %s",
				 g_get_prgname (),
				 CORBA_exception_id (&ev));
		}
	}
	
	CORBA_exception_free (&ev);

	return metafile;
}

static Nautilus_Metafile
get_metafile (NautilusDirectory *directory)
{
	char *uri;

	if (directory->details->metafile_corba_object == CORBA_OBJECT_NIL) {
		uri = nautilus_directory_get_uri (directory);
		directory->details->metafile_corba_object = open_metafile (uri, FALSE);
		g_free (uri);
	}

	return directory->details->metafile_corba_object;	
}

gboolean
nautilus_directory_is_metadata_read (NautilusDirectory *directory)
{
	CORBA_Environment ev;
	gboolean result;
	Nautilus_Metafile metafile;

	g_return_val_if_fail (NAUTILUS_IS_DIRECTORY (directory), FALSE);
	
	CORBA_exception_init (&ev);

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return TRUE;
	}
		
	result = Nautilus_Metafile_is_read (metafile, &ev);
	if (ev._major != CORBA_NO_EXCEPTION) {
		result = TRUE;
		g_debug ("CORBA exception when determining whether metafile is read: %s",
			 CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);

	return result;
}

char *
nautilus_directory_get_file_metadata (NautilusDirectory *directory,
				      const char *file_name,
				      const char *key,
				      const char *default_metadata)
{
	CORBA_Environment ev;
	char             *result;
	const char       *non_null_default;
	CORBA_char       *corba_value;
	Nautilus_Metafile metafile;

	g_return_val_if_fail (NAUTILUS_IS_DIRECTORY (directory), g_strdup (default_metadata));
	g_return_val_if_fail (!eel_str_is_empty (file_name), g_strdup (default_metadata));
	g_return_val_if_fail (!eel_str_is_empty (key), g_strdup (default_metadata));
	
	/* We can't pass NULL as a CORBA_string - pass "" instead. */
	non_null_default = default_metadata != NULL ? default_metadata : "";

	CORBA_exception_init (&ev);

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return g_strdup (default_metadata);
	}

	corba_value = Nautilus_Metafile_get (metafile, file_name, key, non_null_default, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when getting file metadata: %s", CORBA_exception_id (&ev));
		CORBA_exception_free (&ev);
		return g_strdup (default_metadata);
	}

	CORBA_exception_free (&ev);

	if (eel_str_is_empty (corba_value)) {
		/* Even though in all other respects we treat "" as NULL, we want to
		 * make sure the caller gets back the same default that was passed in.
		 */
		result = g_strdup (default_metadata);
	} else {
		result = g_strdup (corba_value);
	}

	CORBA_free (corba_value);

	return result;
}

GList *
nautilus_directory_get_file_metadata_list (NautilusDirectory *directory,
					   const char *file_name,
					   const char *list_key,
					   const char *list_subkey)
{
	CORBA_Environment      ev;
	GList                 *result;
	Nautilus_MetadataList *corba_value;
	CORBA_unsigned_long    buf_pos;
	Nautilus_Metafile      metafile;

	g_return_val_if_fail (NAUTILUS_IS_DIRECTORY (directory), NULL);
	g_return_val_if_fail (!eel_str_is_empty (file_name), NULL);
	g_return_val_if_fail (!eel_str_is_empty (list_key), NULL);
	g_return_val_if_fail (!eel_str_is_empty (list_subkey), NULL);
	
	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return NULL;
	}

	CORBA_exception_init (&ev);
	
	corba_value = Nautilus_Metafile_get_list (metafile, file_name, list_key, list_subkey, &ev);
	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("Failed to get metafile list: %s\n", CORBA_exception_id (&ev));
		CORBA_exception_free (&ev);
		return NULL;
	}
	
	CORBA_exception_free (&ev);

	result = NULL;
	for (buf_pos = 0; buf_pos < corba_value->_length; ++buf_pos) {
		result = g_list_prepend (result, g_strdup (corba_value->_buffer [buf_pos]));
	}
	result = g_list_reverse (result);
	CORBA_free (corba_value);

	return result;
}

void
nautilus_directory_set_file_metadata (NautilusDirectory *directory,
				      const char *file_name,
				      const char *key,
				      const char *default_metadata,
				      const char *metadata)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));
	g_return_if_fail (!eel_str_is_empty (file_name));
	g_return_if_fail (!eel_str_is_empty (key));
	
	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}
	
	/* We can't pass NULL as a CORBA_string - pass "" instead.
	 */
	if (default_metadata == NULL) {
		default_metadata = "";
	}
	if (metadata == NULL) {
		metadata = "";
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_set (metafile, file_name, key, default_metadata, metadata, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when setting file metadata: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);
}

void
nautilus_directory_set_file_metadata_list (NautilusDirectory *directory,
					   const char *file_name,
					   const char *list_key,
					   const char *list_subkey,
					   GList *list)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;
	Nautilus_MetadataList *corba_list;
	int	len;
	int	buf_pos;
	GList   *list_ptr;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));
	g_return_if_fail (!eel_str_is_empty (file_name));
	g_return_if_fail (!eel_str_is_empty (list_key));
	g_return_if_fail (!eel_str_is_empty (list_subkey));

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	len = g_list_length (list);
	
	corba_list = Nautilus_MetadataList__alloc ();
	corba_list->_maximum = len;
	corba_list->_length  = len;
	corba_list->_buffer  = CORBA_sequence_CORBA_string_allocbuf (len);

	/* We allocate our buffer with CORBA calls, so CORBA_free will clean it
	 * all up if we set release to TRUE.
	 */
	CORBA_sequence_set_release (corba_list, CORBA_TRUE);

	buf_pos  = 0;
	list_ptr = list;
	while (list_ptr != NULL) {
		corba_list->_buffer [buf_pos] = CORBA_string_dup (list_ptr->data);
		list_ptr = g_list_next (list_ptr);
		++buf_pos;
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_set_list (metafile, file_name, list_key, list_subkey, corba_list, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when setting file metadata list: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);

	CORBA_free (corba_list);
}

gboolean 
nautilus_directory_get_boolean_file_metadata (NautilusDirectory *directory,
					      const char *file_name,
					      const char *key,
					      gboolean default_metadata)
{
	char *result_as_string;
	gboolean result;

	result_as_string = nautilus_directory_get_file_metadata
		(directory, file_name, key,
		 default_metadata ? "true" : "false");
	g_assert (result_as_string != NULL);
	
	if (g_ascii_strcasecmp (result_as_string, "true") == 0) {
		result = TRUE;
	} else if (g_ascii_strcasecmp (result_as_string, "false") == 0) {
		result = FALSE;
	} else {
		g_error ("boolean metadata with value other than true or false");
		result = default_metadata;
	}

	g_free (result_as_string);
	return result;
}

void
nautilus_directory_set_boolean_file_metadata (NautilusDirectory *directory,
					      const char *file_name,
					      const char *key,
					      gboolean default_metadata,
					      gboolean metadata)
{
	nautilus_directory_set_file_metadata
		(directory, file_name, key,
		 default_metadata ? "true" : "false",
		 metadata ? "true" : "false");
}

int 
nautilus_directory_get_integer_file_metadata (NautilusDirectory *directory,
					      const char *file_name,
					      const char *key,
					      int default_metadata)
{
	char *result_as_string;
	char *default_as_string;
	int result;
	char c;

	default_as_string = g_strdup_printf ("%d", default_metadata);
	result_as_string = nautilus_directory_get_file_metadata
		(directory, file_name, key, default_as_string);

	/* Normally we can't get a a NULL, but we check for it here to
	 * handle the oddball case of a non-existent directory.
	 */
	if (result_as_string == NULL) {
		result = default_metadata;
	} else {
		if (sscanf (result_as_string, " %d %c", &result, &c) != 1) {
			result = default_metadata;
		}
		g_free (result_as_string);
	}

	g_free (default_as_string);
	return result;
}

void
nautilus_directory_set_integer_file_metadata (NautilusDirectory *directory,
					      const char *file_name,
					      const char *key,
					      int default_metadata,
					      int metadata)
{
	char *value_as_string;
	char *default_as_string;

	value_as_string = g_strdup_printf ("%d", metadata);
	default_as_string = g_strdup_printf ("%d", default_metadata);

	nautilus_directory_set_file_metadata
		(directory, file_name, key,
		 default_as_string, value_as_string);

	g_free (value_as_string);
	g_free (default_as_string);
}

void
nautilus_directory_copy_file_metadata (NautilusDirectory *source_directory,
				       const char *source_file_name,
				       NautilusDirectory *destination_directory,
				       const char *destination_file_name)
{
	CORBA_Environment ev;
	char *destination_uri;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (source_directory));
	g_return_if_fail (source_file_name != NULL);
	g_return_if_fail (NAUTILUS_IS_DIRECTORY (destination_directory));
	g_return_if_fail (destination_file_name != NULL);

	metafile = get_metafile (source_directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	destination_uri = nautilus_directory_get_uri (destination_directory);

	CORBA_exception_init (&ev);

	Nautilus_Metafile_copy (metafile, source_file_name,
				destination_uri, destination_file_name, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when copying file metadata: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);

	g_free (destination_uri);
}

void
nautilus_directory_remove_file_metadata (NautilusDirectory *directory,
					 const char *file_name)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));
	g_return_if_fail (file_name != NULL);

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_remove (metafile, file_name, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when removing file metadata: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);
}

void
nautilus_directory_rename_file_metadata (NautilusDirectory *directory,
					 const char *old_file_name,
					 const char *new_file_name)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));
	g_return_if_fail (old_file_name != NULL);
	g_return_if_fail (new_file_name != NULL);

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_rename (metafile, old_file_name, new_file_name, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when renaming file metadata: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);
}

void
nautilus_directory_rename_directory_metadata (NautilusDirectory *directory,
					      const char *new_directory_uri)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));
	g_return_if_fail (new_directory_uri != NULL);

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_rename_directory (metafile, new_directory_uri, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when renaming directory metadata: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);
}

void
nautilus_directory_register_metadata_monitor (NautilusDirectory *directory)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));

	if (directory->details->metafile_monitor != NULL) {
		/* If there's already a monitor, it's already registered. */
		return;
	}
	
	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	directory->details->metafile_monitor = nautilus_metafile_monitor_new (directory);

	CORBA_exception_init (&ev);

	Nautilus_Metafile_register_monitor
		(metafile,
		 BONOBO_OBJREF (directory->details->metafile_monitor),
		 &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when registering metadata monitor: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);
}

void
nautilus_directory_unregister_metadata_monitor (NautilusDirectory *directory)
{
	CORBA_Environment ev;
	Nautilus_Metafile metafile;

	g_return_if_fail (NAUTILUS_IS_DIRECTORY (directory));	
	g_return_if_fail (NAUTILUS_IS_METAFILE_MONITOR (directory->details->metafile_monitor));

	metafile = get_metafile (directory);

	if (metafile == CORBA_OBJECT_NIL) {
		return;
	}

	CORBA_exception_init (&ev);

	Nautilus_Metafile_unregister_monitor
		(metafile,
		 BONOBO_OBJREF (directory->details->metafile_monitor),
		 &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_debug ("CORBA exception when unregistering metadata monitor: %s", CORBA_exception_id (&ev));
	}

	CORBA_exception_free (&ev);

	bonobo_object_unref (directory->details->metafile_monitor);
	directory->details->metafile_monitor = NULL;
}