#include "atspi/atspi.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

pid_t child_pid;
AtspiEventListener *listener;

void
basic (AtspiAccessible *obj)
{
  gchar *str;
  gint count;
  gint i;
  AtspiAccessible *accessible;
  GError *error = NULL;

  str = atspi_accessible_get_name (obj, &error);
  if (str)
    g_free (str);
  accessible = atspi_accessible_get_parent (obj, NULL);
  if (accessible)
    g_object_unref (accessible);
  count = atspi_accessible_get_child_count (obj, &error);
  for (i = 0; i < count; i++)
  {
    accessible = atspi_accessible_get_child_at_index (obj, i, &error);
    if (accessible)
      g_object_unref (accessible);
  }
}

static gboolean
end (void *data)
{
  atspi_event_quit ();
  atspi_exit ();
  exit (0);
}

static gboolean
kill_child (void *data)
{
  kill (child_pid, SIGTERM);
  return FALSE;
}

void
on_event (AtspiEvent *event, void *data)
{
  if (atspi_accessible_get_role (event->source, NULL) == ATSPI_ROLE_DESKTOP_FRAME)
  {
    if (strstr (event->type, "add"))
    {
      AtspiAccessible *desktop = atspi_get_desktop (0);
      guint id;
      basic (desktop);
      g_object_unref (desktop);
      id = g_timeout_add (3000, kill_child, NULL);
      g_source_set_name_by_id (id, "[at-spi2-core] kill_child");
    }
    else
    {
      guint id;
      id = g_idle_add (end, NULL);
      g_source_set_name_by_id (id, "[at-spi2-core] end");
    }
  }
  g_boxed_free (ATSPI_TYPE_EVENT, event);
}

int
main()
{
  atspi_init ();

  listener = atspi_event_listener_new (on_event, NULL, NULL);
  atspi_event_listener_register (listener, "object:children-changed", NULL);
  child_pid = fork ();
  if (!child_pid)
    execlp ("gedit", "gedit", NULL);
  atspi_event_main ();
  return 0;
}
