/*
 * AT-SPI - Assistive Technology Service Provider Interface
 * (Gnome Accessibility Project; http://developer.gnome.org/projects/gap)
 *
 * Copyright 2002 Ximian, Inc.
 *           2002 Sun Microsystems Inc.
 *           
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

#ifndef _ATSPI_COMPONENT_H_
#define _ATSPI_COMPONENT_H_

#include "glib-object.h"

#include "atspi-accessible.h"
#include "atspi-constants.h"

typedef struct _AtspiRect AtspiRect;
struct _AtspiRect
{
  gint x;
  gint y;
  gint width;
  gint height;
};

/**
 * ATSPI_TYPE_RECT:
 * 
 * The #GType for a boxed type holding a #AtspiRect.
 */
#define	ATSPI_TYPE_RECT (atspi_rect_get_type ())

typedef struct _AtspiPoint AtspiPoint;
struct _AtspiPoint
{
  gint x;
  gint y;
};

/**
 * ATSPI_TYPE_POINT:
 * 
 * The #GType for a boxed type holding a #AtspiPoint.
 */
#define	ATSPI_TYPE_POINT (atspi_point_get_type ())

#define ATSPI_TYPE_COMPONENT                    (atspi_component_get_type ())
#define ATSPI_IS_COMPONENT(obj)                 G_TYPE_CHECK_INSTANCE_TYPE ((obj), ATSPI_TYPE_COMPONENT)
#define ATSPI_COMPONENT(obj)                    G_TYPE_CHECK_INSTANCE_CAST ((obj), ATSPI_TYPE_COMPONENT, AtspiComponent)
#define ATSPI_COMPONENT_GET_IFACE(obj)          (G_TYPE_INSTANCE_GET_INTERFACE ((obj), ATSPI_TYPE_COMPONENT, AtspiComponent))

GType atspi_component_get_type ();

struct _AtspiComponent
{
  GTypeInterface parent;
  gboolean (*contains) (AtspiComponent *component, gint x, gint y, AtspiCoordType ctype, GError **error);
  AtspiAccessible *(*ref_accessible_at_point) (AtspiComponent *component, gint x, gint y, AtspiCoordType ctype, GError **error);
  AtspiRect (*get_extents) (AtspiComponent *component, gint *x, gint *y, gint *width, gint *height, AtspiCoordType ctype, GError **error);
  AtspiPoint (*get_position) (AtspiComponent *component, AtspiCoordType ctype, GError **error);
  AtspiPoint (*get_size) (AtspiComponent *component, GError **error);
  AtspiComponentLayer (*get_layer) (AtspiComponent *component, GError **error);

  gshort (*get_mdi_z_order) (AtspiComponent *component, GError **error);
  gboolean (*grab_focus) (AtspiComponent *component, GError **error);
  gdouble (*get_alpha)    (AtspiComponent *component, GError **error);
};

gboolean atspi_component_contains (AtspiComponent *obj, gint x, gint y, AtspiCoordType ctype, GError **error);

AtspiAccessible *atspi_component_ref_accessible_at_point (AtspiComponent *obj, gint x, gint y, AtspiCoordType ctype, GError **error);

AtspiRect atspi_component_get_extents (AtspiComponent *obj, gint *x, gint *y, gint *width, gint *height, AtspiCoordType ctype, GError **error);

AtspiPoint atspi_component_get_position (AtspiComponent *obj, AtspiCoordType ctype, GError **error);

AtspiPoint atspi_component_get_size (AtspiComponent *obj, GError **error);

AtspiComponentLayer atspi_component_get_layer (AtspiComponent *obj, GError **error);

gshort atspi_component_get_mdi_z_order (AtspiComponent *obj, GError **error);

gboolean atspi_component_grab_focus (AtspiComponent *obj, GError **error);

gdouble      atspi_component_get_alpha    (AtspiComponent *obj, GError **error);
#endif	/* _ATSPI_COMPONENT_H_ */