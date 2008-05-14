/*
 * AT-SPI - Assistive Technology Service Provider Interface
 * (Gnome Accessibility Project; http://developer.gnome.org/projects/gap)
 *
 * Copyright 2008 Novell, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "accessible.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GHashTable *path2ptr;
static guint objindex;

static void
deregister_object (gpointer data, GObject *obj)
{
  spi_dbus_notify_remove(ATK_OBJECT(obj), NULL);
  g_hash_table_remove (path2ptr, &obj);
}

static guint
register_object (AtkObject * obj)
{
  gint *new_int;

  if (!path2ptr)
    {
      path2ptr = g_hash_table_new_full (g_int_hash, g_int_equal, g_free, NULL);
      if (!path2ptr)
	return ++objindex;
    }
  objindex++;
  while (g_hash_table_lookup (path2ptr, &objindex))
    {
      objindex++;
      /* g_object_get_data returning 0 means no data, so handle wrap-around */
      if (objindex == 0)
	objindex++;
    }
  new_int = (gint *)g_malloc(sizeof(gint));
  if (new_int)
  {
    *new_int = objindex;
    g_hash_table_insert (path2ptr, new_int, obj);
  }
  g_object_set_data (G_OBJECT (obj), "dbus-id", (gpointer) objindex);
  g_object_weak_ref(G_OBJECT(obj), deregister_object, NULL);
  spi_dbus_notify_change(obj, TRUE, NULL);
  return objindex;
}

AtkObject *
spi_dbus_get_object (const char *path)
{
  guint index;
  void *data;

  g_assert (path);
  if (strncmp(path, "/org/freedesktop/atspi/accessible", 33) != 0) return NULL;
  path += 33;	/* skip over preamble */
  if (path[0] == '\0') return atk_get_root();
  if (path[0] != '/') return NULL;
  path++;
  index = atoi (path);
  data = g_hash_table_lookup (path2ptr, &index);
  if (data)
    return ATK_OBJECT (data);
  return NULL;
}

gchar *
spi_dbus_get_path (AtkObject * obj)
{
  if (!obj) return NULL;
  guint index = (guint) g_object_get_data (G_OBJECT (obj), "dbus-id");
  if (!index)
    index = register_object (obj);
  return g_strdup_printf ("/org/freedesktop/atspi/accessible/%d", index);
}

DBusMessage *
spi_dbus_general_error (DBusMessage * message)
{
  return dbus_message_new_error (message,
				 "org.freedesktop.atspi.GeneralError",
				 "General error");
}

/* Reply with the given object and dereference it if unref is TRUE */
DBusMessage *
spi_dbus_return_object (DBusMessage * message, AtkObject * obj, int unref)
{
  DBusMessage *reply;
  const char *path = spi_dbus_get_path (obj);
  if (unref)
    g_object_unref (obj);
  if (!path)
    {
      /* Should we have a more specific error for this? */
      return spi_dbus_general_error (message);
    }
  reply = dbus_message_new_method_return (message);
  if (reply)
    {
      dbus_message_append_args (reply, DBUS_TYPE_OBJECT_PATH, path,
				DBUS_TYPE_INVALID);
    }
  return reply;
}

DBusMessage *
spi_dbus_return_rect (DBusMessage * message, gint ix, gint iy, gint iwidth,
		      gint iheight)
{
  DBusMessage *reply;
  dbus_uint32_t x, y, width, height;

  x = ix;
  y = iy;
  width = iwidth;
  height = iheight;
  reply = dbus_message_new_method_return (message);
  if (reply)
    {
      DBusMessageIter iter, sub;
      dbus_message_iter_init_append (reply, &iter);
      if (!dbus_message_iter_open_container
	  (&iter, DBUS_TYPE_STRUCT, NULL, &sub))
	goto oom;
      dbus_message_iter_append_basic (&sub, DBUS_TYPE_INT32, &x);
      dbus_message_iter_append_basic (&sub, DBUS_TYPE_INT32, &y);
      dbus_message_iter_append_basic (&sub, DBUS_TYPE_INT32, &width);
      dbus_message_iter_append_basic (&sub, DBUS_TYPE_INT32, &height);
      if (!dbus_message_iter_close_container (&iter, &sub))
	goto oom;
    }
  return reply;
oom:
  /* todo: return an error */
  return reply;
}

dbus_bool_t
spi_dbus_return_v_object (DBusMessageIter * iter, AtkObject * obj, int unref)
{
  const char *path = spi_dbus_get_path (obj);
  if (unref)
    g_object_unref (obj);
  if (!path)
    return FALSE;
  return droute_return_v_object (iter, path);
}


void
spi_dbus_initialize (DRouteData * data)
{
  spi_initialize_accessible (data);
  spi_initialize_action(data);
  spi_initialize_component (data);
  spi_initialize_document (data);
  spi_initialize_editabletext (data);
  spi_initialize_hyperlink (data);
  spi_initialize_hypertext (data);
  spi_initialize_image (data);
  spi_initialize_selection (data);
  spi_initialize_table (data);
  spi_initialize_text (data);
  spi_initialize_value (data);
  spi_initialize_introspectable(data);
}

void spi_dbus_emit_valist(DBusConnection *bus, const char *path, const char *interface, const char *name, int first_arg_type, va_list args)
{
  DBusMessage *sig;

  sig = dbus_message_new_signal(path, interface, name);
  if (first_arg_type != DBUS_TYPE_INVALID)
  {
    dbus_message_append_args_valist(sig, first_arg_type, args);
  }
  dbus_connection_send(bus, sig, NULL);
  dbus_message_unref(sig);
}

static GString *
spi_get_tree (AtkObject * obj, GString * str, DRouteData * data)
{
  int role;
  const char *name;
  gchar *path;
  GSList *l;
  gint childcount;
  gint i;

  if (!obj)
    return NULL;
  role = spi_accessible_role_from_atk_role (atk_object_get_role (obj));;
  name = atk_object_get_name (obj);
  if (!name)
    name = "";
  path = spi_dbus_get_path (obj);
  g_string_append_printf (str,
			  "<object path=\"%s\" name=\"%s\" role=\"%d\">\n",
			  path, name, role);
  for (l = data->interfaces; l; l = g_slist_next (l))
    {
      DRouteInterface *iface_def = (DRouteInterface *) l->data;
      void *datum = NULL;
      if (iface_def->get_datum)
	datum = (*iface_def->get_datum) (path, data->user_data);
      if (datum)
	{
	  g_string_append_printf (str, "<interface name=\"%s\"/>\n",
				  iface_def->name);
	  if (iface_def->free_datum)
	    (*iface_def->free_datum) (datum);
	}
    }
  childcount = atk_object_get_n_accessible_children (obj);
  for (i = 0; i < childcount; i++)
    {
      AtkObject *child = atk_object_ref_accessible_child (obj, i);
      str = spi_get_tree (child, str, data);
      g_object_unref (child);
    }
  str = g_string_append (str, "</object>\n");
  return str;
}

dbus_bool_t spi_dbus_message_iter_get_struct(DBusMessageIter *iter, ...)
{
  va_list args;
  DBusMessageIter iter_struct;
  int type;
  void *ptr;

  dbus_message_iter_recurse(iter, &iter_struct);
  va_start(args, iter);
  for (;;)
  {
    type = va_arg(args, int);
    if (type == DBUS_TYPE_INVALID) break;
    if (type != dbus_message_iter_get_arg_type(&iter_struct))
    {
      va_end(args);
      return FALSE;
    }
    ptr = va_arg(args, void *);
    dbus_message_iter_get_basic(&iter_struct, ptr);
    dbus_message_iter_next(&iter_struct);
  }
  dbus_message_iter_next(iter);
  va_end(args);
  return TRUE;
}

dbus_bool_t spi_dbus_message_iter_append_struct(DBusMessageIter *iter, ...)
{
  va_list args;
  DBusMessageIter iter_struct;
  int type;
  void *ptr;

  if (!dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &iter_struct)) return FALSE;
  va_start(args, iter);
  for (;;)
  {
    type = va_arg(args, int);
    if (type == DBUS_TYPE_INVALID) break;
    ptr = va_arg(args, void *);
    dbus_message_iter_append_basic(&iter_struct, type, ptr);
  }
  if (!dbus_message_iter_close_container(iter, &iter_struct)) return FALSE;
  va_end(args);
  return TRUE;
}

dbus_bool_t spi_dbus_marshall_deviceEvent(DBusMessage *message, const Accessibility_DeviceEvent *e)
{
  DBusMessageIter iter;

  if (!message) return FALSE;
  dbus_message_iter_init_append(message, &iter);
  return spi_dbus_message_iter_append_struct(&iter, DBUS_TYPE_UINT32, &e->type, DBUS_TYPE_INT32, &e->id, DBUS_TYPE_INT16, &e->hw_code, DBUS_TYPE_INT16, &e->modifiers, DBUS_TYPE_INT32, &e->timestamp, DBUS_TYPE_STRING, &e->event_string, DBUS_TYPE_BOOLEAN, &e->is_text, DBUS_TYPE_INVALID);
}

dbus_bool_t spi_dbus_demarshall_deviceEvent(DBusMessage *message, Accessibility_DeviceEvent *e)
{
  DBusMessageIter iter;

  dbus_message_iter_init(message, &iter);
  return spi_dbus_message_iter_get_struct(&iter, DBUS_TYPE_UINT32, &e->type, DBUS_TYPE_INT32, &e->id, DBUS_TYPE_INT16, &e->hw_code, DBUS_TYPE_INT16, &e->modifiers, DBUS_TYPE_INT32, &e->timestamp, DBUS_TYPE_STRING, &e->event_string, DBUS_TYPE_BOOLEAN, &e->is_text, DBUS_TYPE_INVALID);
}
