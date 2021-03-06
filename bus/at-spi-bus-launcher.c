/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * 
 * at-spi-bus-launcher: Manage the a11y bus as a child process 
 *
 * Copyright 2011-2018 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <unistd.h>
#include <string.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>

#include <gio/gio.h>
#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

typedef enum {
  A11Y_BUS_STATE_IDLE = 0,
  A11Y_BUS_STATE_READING_ADDRESS,
  A11Y_BUS_STATE_RUNNING,
  A11Y_BUS_STATE_ERROR
} A11yBusState;

typedef struct {
  GMainLoop *loop;
  gboolean launch_immediately;
  gboolean a11y_enabled;
  gboolean screen_reader_enabled;
  GDBusConnection *session_bus;
  GSettings *a11y_schema;
  GSettings *interface_schema;
  int name_owner_id;

  GDBusProxy *client_proxy;

  A11yBusState state;
  /* -1 == error, 0 == pending, > 0 == running */
  int a11y_bus_pid;
  char *a11y_bus_address;
#ifdef HAVE_X11
  gboolean x11_prop_set;
#endif
  int pipefd[2];
  int listenfd;
  char *a11y_launch_error_message;
  GDBusProxy *sm_proxy;
} A11yBusLauncher;

static A11yBusLauncher *_global_app = NULL;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.a11y.Bus'>"
  "    <method name='GetAddress'>"
  "      <arg type='s' name='address' direction='out'/>"
  "    </method>"
  "  </interface>"
  "<interface name='org.a11y.Status'>"
  "<property name='IsEnabled' type='b' access='readwrite'/>"
  "<property name='ScreenReaderEnabled' type='b' access='readwrite'/>"
  "</interface>"
  "</node>";
static GDBusNodeInfo *introspection_data = NULL;

static void
respond_to_end_session (GDBusProxy *proxy)
{
  GVariant *parameters;

  parameters = g_variant_new ("(bs)", TRUE, "");

  g_dbus_proxy_call (proxy,
                     "EndSessionResponse", parameters,
                     G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, NULL, NULL);
}

static void
g_signal_cb (GDBusProxy *proxy,
             gchar      *sender_name,
             gchar      *signal_name,
             GVariant   *parameters,
             gpointer    user_data)
{
  A11yBusLauncher *app = user_data;

  if (g_strcmp0 (signal_name, "QueryEndSession") == 0)
    respond_to_end_session (proxy);
  else if (g_strcmp0 (signal_name, "EndSession") == 0)
    respond_to_end_session (proxy);
  else if (g_strcmp0 (signal_name, "Stop") == 0)
    g_main_loop_quit (app->loop);
}

static void
client_proxy_ready_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  A11yBusLauncher *app = user_data;
  GError *error = NULL;

  app->client_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

  if (error != NULL)
    {
      g_warning ("Failed to get a client proxy: %s", error->message);
      g_error_free (error);

      return;
    }

  g_signal_connect (app->client_proxy, "g-signal",
                    G_CALLBACK (g_signal_cb), app);
}

static void
client_registered (GObject *source,
                   GAsyncResult *result,
                   gpointer user_data)
{
  A11yBusLauncher *app = user_data;
  GError *error = NULL;
  GVariant *variant;
  gchar *object_path;
  GDBusProxyFlags flags;

  variant = g_dbus_proxy_call_finish (app->sm_proxy, result, &error);
  if (!variant)
    {
      if (error != NULL)
        {
          g_warning ("Failed to register client: %s", error->message);
          g_error_free (error);
        }
    }
  else
    {
      g_variant_get (variant, "(o)", &object_path);
      g_variant_unref (variant);

      flags = G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES;
      g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, flags, NULL,
                                "org.gnome.SessionManager", object_path,
                                "org.gnome.SessionManager.ClientPrivate",
                                NULL, client_proxy_ready_cb, app);

      g_free (object_path);
    }
  g_clear_object (&app->sm_proxy);
}

static void
register_client (A11yBusLauncher *app)
{
  GDBusProxyFlags flags;
  GError *error;
  const gchar *app_id;
  const gchar *autostart_id;
  gchar *client_startup_id;
  GVariant *parameters;

  flags = G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
          G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS;

  error = NULL;
  app->sm_proxy = g_dbus_proxy_new_sync (app->session_bus, flags, NULL,
                                         "org.gnome.SessionManager",
                                         "/org/gnome/SessionManager",
                                         "org.gnome.SessionManager",
                                         NULL, &error);

  if (error != NULL)
    {
      g_warning ("Failed to get session manager proxy: %s", error->message);
      g_error_free (error);

      return;
    }

  app_id = "at-spi-bus-launcher";
  autostart_id = g_getenv ("DESKTOP_AUTOSTART_ID");

  if (autostart_id != NULL)
    {
      client_startup_id = g_strdup (autostart_id);
      g_unsetenv ("DESKTOP_AUTOSTART_ID");
    }
  else
    {
      client_startup_id = g_strdup ("");
    }

  parameters = g_variant_new ("(ss)", app_id, client_startup_id);
  g_free (client_startup_id);

  error = NULL;
  g_dbus_proxy_call (app->sm_proxy,
                     "RegisterClient", parameters,
                     G_DBUS_CALL_FLAGS_NONE,
                     G_MAXINT, NULL, client_registered, app);

}

static void
name_appeared_handler (GDBusConnection *connection,
                       const gchar     *name,
                       const gchar     *name_owner,
                       gpointer         user_data)
{
  A11yBusLauncher *app = user_data;

  register_client (app);
}

/**
 * unix_read_all_fd_to_string:
 *
 * Read all data from a file descriptor to a C string buffer.
 */
static gboolean
unix_read_all_fd_to_string (int      fd,
                            char    *buf,
                            ssize_t  max_bytes)
{
  ssize_t bytes_read;

  while (max_bytes > 1 && (bytes_read = read (fd, buf, MIN (4096, max_bytes - 1))))
    {
      if (bytes_read < 0)
        return FALSE;
      buf += bytes_read;
      max_bytes -= bytes_read;
    }
  *buf = '\0';
  return TRUE;
}

static void
on_bus_exited (GPid     pid,
               gint     status,
               gpointer data)
{
  A11yBusLauncher *app = data;
  
  app->a11y_bus_pid = -1;
  app->state = A11Y_BUS_STATE_ERROR;
  if (app->a11y_launch_error_message == NULL)
    {
      if (WIFEXITED (status))
        app->a11y_launch_error_message = g_strdup_printf ("Bus exited with code %d", WEXITSTATUS (status));
      else if (WIFSIGNALED (status))
        app->a11y_launch_error_message = g_strdup_printf ("Bus killed by signal %d", WTERMSIG (status));
      else if (WIFSTOPPED (status))
        app->a11y_launch_error_message = g_strdup_printf ("Bus stopped by signal %d", WSTOPSIG (status));
    }
  g_main_loop_quit (app->loop);
} 

#ifdef DBUS_DAEMON
static void
setup_bus_child_daemon (gpointer data)
{
  A11yBusLauncher *app = data;
  (void) app;

  close (app->pipefd[0]);
  dup2 (app->pipefd[1], 3);
  close (app->pipefd[1]);

  /* On Linux, tell the bus process to exit if this process goes away */
#ifdef __linux__
  prctl (PR_SET_PDEATHSIG, 15);
#endif
}

static gboolean
ensure_a11y_bus_daemon (A11yBusLauncher *app, char *config_path)
{
  char *argv[] = { DBUS_DAEMON, config_path, "--nofork", "--print-address", "3", NULL };
  GPid pid;
  char addr_buf[2048];
  GError *error = NULL;

  if (pipe (app->pipefd) < 0)
    g_error ("Failed to create pipe: %s", strerror (errno));

  g_clear_pointer (&app->a11y_launch_error_message, g_free);

  if (!g_spawn_async (NULL,
                      argv,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      setup_bus_child_daemon,
                      app,
                      &pid,
                      &error))
    {
      app->a11y_bus_pid = -1;
      app->a11y_launch_error_message = g_strdup (error->message);
      g_clear_error (&error);
      goto error;
    }

  close (app->pipefd[1]);
  app->pipefd[1] = -1;

  g_child_watch_add (pid, on_bus_exited, app);

  app->state = A11Y_BUS_STATE_READING_ADDRESS;
  app->a11y_bus_pid = pid;
  g_debug ("Launched a11y bus, child is %ld", (long) pid);
  if (!unix_read_all_fd_to_string (app->pipefd[0], addr_buf, sizeof (addr_buf)))
    {
      app->a11y_launch_error_message = g_strdup_printf ("Failed to read address: %s", strerror (errno));
      kill (app->a11y_bus_pid, SIGTERM);
      app->a11y_bus_pid = -1;
      goto error;
    }
  close (app->pipefd[0]);
  app->pipefd[0] = -1;
  app->state = A11Y_BUS_STATE_RUNNING;

  /* Trim the trailing newline */
  app->a11y_bus_address = g_strchomp (g_strdup (addr_buf));
  g_debug ("a11y bus address: %s", app->a11y_bus_address);

  return TRUE;

error:
  close (app->pipefd[0]);
  close (app->pipefd[1]);
  app->state = A11Y_BUS_STATE_ERROR;

  return FALSE;
}
#else
static gboolean
ensure_a11y_bus_daemon (A11yBusLauncher *app, char *config_path)
{
	return FALSE;
}
#endif

#ifdef DBUS_BROKER
static void
setup_bus_child_broker (gpointer data)
{
  A11yBusLauncher *app = data;
  gchar *pid_str;
  (void) app;

  dup2 (app->listenfd, 3);
  close (app->listenfd);
  g_setenv("LISTEN_FDS", "1", TRUE);

  pid_str = g_strdup_printf("%u", getpid());
  g_setenv("LISTEN_PID", pid_str, TRUE);
  g_free(pid_str);

  /* Tell the bus process to exit if this process goes away */
  prctl (PR_SET_PDEATHSIG, SIGTERM);
}

static gboolean
ensure_a11y_bus_broker (A11yBusLauncher *app, char *config_path)
{
  char *argv[] = { DBUS_BROKER, config_path, "--scope", "user", NULL };
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  socklen_t addr_len = sizeof(addr);
  GPid pid;
  GError *error = NULL;

  if ((app->listenfd = socket (PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
    g_error ("Failed to create listening socket: %s", strerror (errno));

  if (bind (app->listenfd, (struct sockaddr *)&addr, sizeof(sa_family_t)) < 0)
    g_error ("Failed to bind listening socket: %s", strerror (errno));

  if (getsockname (app->listenfd, (struct sockaddr *)&addr, &addr_len) < 0)
    g_error ("Failed to get socket name for listening socket: %s", strerror(errno));

  if (listen (app->listenfd, 1024) < 0)
    g_error ("Failed to listen on socket: %s", strerror(errno));

  g_clear_pointer (&app->a11y_launch_error_message, g_free);

  if (!g_spawn_async (NULL,
                      argv,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      setup_bus_child_broker,
                      app,
                      &pid,
                      &error))
    {
      app->a11y_bus_pid = -1;
      app->a11y_launch_error_message = g_strdup (error->message);
      g_clear_error (&error);
      goto error;
    }

  close (app->listenfd);
  app->listenfd = -1;

  g_child_watch_add (pid, on_bus_exited, app);
  app->a11y_bus_pid = pid;
  g_debug ("Launched a11y bus, child is %ld", (long) pid);
  app->state = A11Y_BUS_STATE_RUNNING;

  app->a11y_bus_address = g_strconcat("unix:abstract=", addr.sun_path + 1, NULL);
  g_debug ("a11y bus address: %s", app->a11y_bus_address);

  return TRUE;

error:
  close (app->listenfd);
  app->state = A11Y_BUS_STATE_ERROR;

  return FALSE;
}
#else
static gboolean
ensure_a11y_bus_broker (A11yBusLauncher *app, char *config_path)
{
	return FALSE;
}
#endif

static gboolean
ensure_a11y_bus (A11yBusLauncher *app)
{
  char *config_path = NULL;
  gboolean success = FALSE;

  if (app->a11y_bus_pid != 0)
    return FALSE;

  if (g_file_test (SYSCONFDIR"/at-spi2/accessibility.conf", G_FILE_TEST_EXISTS))
      config_path = "--config-file="SYSCONFDIR"/at-spi2/accessibility.conf";
  else
      config_path = "--config-file="DATADIR"/defaults/at-spi2/accessibility.conf";

#ifdef WANT_DBUS_BROKER
    success = ensure_a11y_bus_broker (app, config_path);
    if (!success)
      {
        if (!ensure_a11y_bus_daemon (app, config_path))
            return FALSE;
      }
#else
    success = ensure_a11y_bus_daemon (app, config_path);
    if (!success)
      {
        if (!ensure_a11y_bus_broker (app, config_path))
            return FALSE;
      }
#endif

#ifdef HAVE_X11
  if (g_getenv ("DISPLAY") != NULL && g_getenv ("WAYLAND_DISPLAY") == NULL)
    {
      Display *display = XOpenDisplay (NULL);
      if (display)
        {
          Atom bus_address_atom = XInternAtom (display, "AT_SPI_BUS", False);
          XChangeProperty (display,
                           XDefaultRootWindow (display),
                           bus_address_atom,
                           XA_STRING, 8, PropModeReplace,
                           (guchar *) app->a11y_bus_address, strlen (app->a11y_bus_address));
          XFlush (display);
          XCloseDisplay (display);
          app->x11_prop_set = TRUE;
        }
    }
#endif

  return TRUE;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  A11yBusLauncher *app = user_data;

  if (g_strcmp0 (method_name, "GetAddress") == 0)
    {
      ensure_a11y_bus (app);
      if (app->a11y_bus_pid > 0)
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(s)", app->a11y_bus_address));
      else
        g_dbus_method_invocation_return_dbus_error (invocation,
                                                    "org.a11y.Bus.Error",
                                                    app->a11y_launch_error_message);
    }
}

static GVariant *
handle_get_property  (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *property_name,
                    GError **error,
                    gpointer               user_data)
{
  A11yBusLauncher *app = user_data;

  if (g_strcmp0 (property_name, "IsEnabled") == 0)
    return g_variant_new ("b", app->a11y_enabled);
  else if (g_strcmp0 (property_name, "ScreenReaderEnabled") == 0)
    return g_variant_new ("b", app->screen_reader_enabled);
  else
    return NULL;
}

static void
handle_a11y_enabled_change (A11yBusLauncher *app, gboolean enabled,
                               gboolean notify_gsettings)
{
  GVariantBuilder builder;
  GVariantBuilder invalidated_builder;

  if (enabled == app->a11y_enabled)
    return;

  app->a11y_enabled = enabled;

  if (notify_gsettings && app->interface_schema)
    {
      g_settings_set_boolean (app->interface_schema, "toolkit-accessibility",
                              enabled);
      g_settings_sync ();
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
  g_variant_builder_init (&invalidated_builder, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&builder, "{sv}", "IsEnabled",
                         g_variant_new_boolean (enabled));

  g_dbus_connection_emit_signal (app->session_bus, NULL, "/org/a11y/bus",
                                 "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", "org.a11y.Status",
                                                &builder,
                                                &invalidated_builder),
                                 NULL);

  g_variant_builder_clear (&builder);
  g_variant_builder_clear (&invalidated_builder);
}

static void
handle_screen_reader_enabled_change (A11yBusLauncher *app, gboolean enabled,
                               gboolean notify_gsettings)
{
  GVariantBuilder builder;
  GVariantBuilder invalidated_builder;

  if (enabled == app->screen_reader_enabled)
    return;

  /* If the screen reader is being enabled, we should enable accessibility
   * if it isn't enabled already */
  if (enabled)
    handle_a11y_enabled_change (app, enabled, notify_gsettings);

  app->screen_reader_enabled = enabled;

  if (notify_gsettings && app->a11y_schema)
    {
      g_settings_set_boolean (app->a11y_schema, "screen-reader-enabled",
                              enabled);
      g_settings_sync ();
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
  g_variant_builder_init (&invalidated_builder, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&builder, "{sv}", "ScreenReaderEnabled",
                         g_variant_new_boolean (enabled));

  g_dbus_connection_emit_signal (app->session_bus, NULL, "/org/a11y/bus",
                                 "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", "org.a11y.Status",
                                                &builder,
                                                &invalidated_builder),
                                 NULL);

  g_variant_builder_clear (&builder);
  g_variant_builder_clear (&invalidated_builder);
}

static gboolean
handle_set_property  (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *property_name,
                      GVariant *value,
                    GError **error,
                    gpointer               user_data)
{
  A11yBusLauncher *app = user_data;
  const gchar *type = g_variant_get_type_string (value);
  gboolean enabled;
  
  if (g_strcmp0 (type, "b") != 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                       "org.a11y.Status.%s expects a boolean but got %s", property_name, type);
      return FALSE;
    }

  enabled = g_variant_get_boolean (value);

  if (g_strcmp0 (property_name, "IsEnabled") == 0)
    {
      handle_a11y_enabled_change (app, enabled, TRUE);
      return TRUE;
    }
  else if (g_strcmp0 (property_name, "ScreenReaderEnabled") == 0)
    {
      handle_screen_reader_enabled_change (app, enabled, TRUE);
      return TRUE;
    }
  else
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                       "Unknown property '%s'", property_name);
      return FALSE;
    }
}

static const GDBusInterfaceVTable bus_vtable =
{
  handle_method_call,
  NULL, /* handle_get_property, */
  NULL  /* handle_set_property */
};

static const GDBusInterfaceVTable status_vtable =
{
  NULL, /* handle_method_call */
  handle_get_property,
  handle_set_property
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  A11yBusLauncher *app = user_data;
  GError *error;
  guint registration_id;
  
  if (connection == NULL)
    {
      g_main_loop_quit (app->loop);
      return;
    }
  app->session_bus = connection;

  error = NULL;
  registration_id = g_dbus_connection_register_object (connection,
                                                       "/org/a11y/bus",
                                                       introspection_data->interfaces[0],
                                                       &bus_vtable,
                                                       _global_app,
                                                       NULL,
                                                       &error);
  if (registration_id == 0)
    {
      g_error ("%s", error->message);
      g_clear_error (&error);
    }

  g_dbus_connection_register_object (connection,
                                     "/org/a11y/bus",
                                     introspection_data->interfaces[1],
                                     &status_vtable,
                                     _global_app,
                                     NULL,
                                     NULL);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  A11yBusLauncher *app = user_data;
  if (app->session_bus == NULL
      && connection == NULL
      && app->a11y_launch_error_message == NULL)
    app->a11y_launch_error_message = g_strdup ("Failed to connect to session bus");
  g_main_loop_quit (app->loop);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  A11yBusLauncher *app = user_data;

  if (app->launch_immediately)
    {
      ensure_a11y_bus (app);
      if (app->state == A11Y_BUS_STATE_ERROR)
        {
          g_main_loop_quit (app->loop);
          return;
        }
    }

  g_bus_watch_name (G_BUS_TYPE_SESSION,
                    "org.gnome.SessionManager",
                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                    name_appeared_handler, NULL,
                    user_data, NULL);
}

static int sigterm_pipefd[2];

static void
sigterm_handler (int signum)
{
  write (sigterm_pipefd[1], "X", 1);
}

static gboolean
on_sigterm_pipe (GIOChannel  *channel,
                 GIOCondition condition,
                 gpointer     data)
{
  A11yBusLauncher *app = data;
  
  g_main_loop_quit (app->loop);

  return FALSE;
}

static void
init_sigterm_handling (A11yBusLauncher *app)
{
  GIOChannel *sigterm_channel;

  if (pipe (sigterm_pipefd) < 0)
    g_error ("Failed to create pipe: %s", strerror (errno));
  signal (SIGTERM, sigterm_handler);

  sigterm_channel = g_io_channel_unix_new (sigterm_pipefd[0]);
  g_io_add_watch (sigterm_channel,
                  G_IO_IN | G_IO_ERR | G_IO_HUP,
                  on_sigterm_pipe,
                  app);
}

static GSettings *
get_schema (const gchar *name)
{
#if GLIB_CHECK_VERSION (2, 32, 0)
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = g_settings_schema_source_lookup (source, name, FALSE);

  if (schema == NULL)
    return NULL;

  return g_settings_new_full (schema, NULL, NULL);
#else
  const char * const *schemas = NULL;
  gint i;

  schemas = g_settings_list_schemas ();
  for (i = 0; schemas[i]; i++)
  {
    if (!strcmp (schemas[i], name))
      return g_settings_new (schemas[i]);
  }

  return NULL;
#endif
}

static void
gsettings_key_changed (GSettings *gsettings, const gchar *key, void *user_data)
{
  gboolean new_val = g_settings_get_boolean (gsettings, key);

  if (!strcmp (key, "toolkit-accessibility"))
    handle_a11y_enabled_change (_global_app, new_val, FALSE);
  else if (!strcmp (key, "screen-reader-enabled"))
    handle_screen_reader_enabled_change (_global_app, new_val, FALSE);
}

int
main (int    argc,
      char **argv)
{
  gboolean a11y_set = FALSE;
  gboolean screen_reader_set = FALSE;
  gint i;

  _global_app = g_slice_new0 (A11yBusLauncher);
  _global_app->loop = g_main_loop_new (NULL, FALSE);

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--launch-immediately"))
        _global_app->launch_immediately = TRUE;
      else if (sscanf (argv[i], "--a11y=%d", &_global_app->a11y_enabled) == 1)
        a11y_set = TRUE;
      else if (sscanf (argv[i], "--screen-reader=%d",
                       &_global_app->screen_reader_enabled) == 1)
        screen_reader_set = TRUE;
    else
      g_error ("usage: %s [--launch-immediately] [--a11y=0|1] [--screen-reader=0|1]", argv[0]);
    }

  _global_app->interface_schema = get_schema ("org.gnome.desktop.interface");
  _global_app->a11y_schema = get_schema ("org.gnome.desktop.a11y.applications");

  if (!a11y_set)
    {
      _global_app->a11y_enabled = _global_app->interface_schema
                                  ? g_settings_get_boolean (_global_app->interface_schema, "toolkit-accessibility")
                                  : _global_app->launch_immediately;
    }

  if (!screen_reader_set)
    {
      _global_app->screen_reader_enabled = _global_app->a11y_schema
                                  ? g_settings_get_boolean (_global_app->a11y_schema, "screen-reader-enabled")
                                  : FALSE;
    }

  if (_global_app->interface_schema)
    g_signal_connect (_global_app->interface_schema,
                      "changed::toolkit-accessibility",
                      G_CALLBACK (gsettings_key_changed), _global_app);

  if (_global_app->a11y_schema)
    g_signal_connect (_global_app->a11y_schema,
                      "changed::screen-reader-enabled",
                      G_CALLBACK (gsettings_key_changed), _global_app);

  init_sigterm_handling (_global_app);

  introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
  g_assert (introspection_data != NULL);

  _global_app->name_owner_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    "org.a11y.Bus",
                    G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                    on_bus_acquired,
                    on_name_acquired,
                    on_name_lost,
                    _global_app,
                    NULL);

  g_main_loop_run (_global_app->loop);

  if (_global_app->a11y_bus_pid > 0)
    kill (_global_app->a11y_bus_pid, SIGTERM);

  /* Clear the X property if our bus is gone; in the case where e.g. 
   * GDM is launching a login on an X server it was using before,
   * we don't want early login processes to pick up the stale address.
   */
#ifdef HAVE_X11
  if (_global_app->x11_prop_set)
    {
      Display *display = XOpenDisplay (NULL);
      if (display)
        {
          Atom bus_address_atom = XInternAtom (display, "AT_SPI_BUS", False);
          XDeleteProperty (display,
                           XDefaultRootWindow (display),
                           bus_address_atom);

          XFlush (display);
          XCloseDisplay (display);
        }
    }
#endif

  if (_global_app->a11y_launch_error_message)
    {
      g_printerr ("Failed to launch bus: %s", _global_app->a11y_launch_error_message);
      return 1;
    }
  return 0;
}
