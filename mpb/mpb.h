/* Copyright (C) 1999, 2000, 2001, 2002, Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MPB_H
#define MPB_H

#include <maxwell.h>

/* this integer flag is defined by main.c from libctl, and is
   set when the user runs the program with --verbose */
extern int verbose;

#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))

#define TWOPI 6.2831853071795864769252867665590057683943388

/**************************************************************************/

extern void get_epsilon_file_func(const char *fname,
				  maxwell_dielectric_function *func,
				  void **func_data);
extern void destroy_epsilon_file_func_data(void *func_data);

extern real linear_interpolate(real rx, real ry, real rz,
			       real *data, int nx, int ny, int nz, int stride);

/**************************************************************************/

/* global variables for retaining data about the eigenvectors between
   calls from Guile: */

#define MAX_NWORK 10
extern int nwork_alloc;

#define NUM_FFT_BANDS 20 /* max number of bands to FFT at a time */

extern maxwell_data *mdata;
extern maxwell_target_data *mtdata;
extern evectmatrix H, W[MAX_NWORK], Hblock;

extern vector3 cur_kvector;
extern scalar_complex *curfield;
extern int curfield_band;
extern char curfield_type;

extern void curfield_reset(void);

/* R[i]/G[i] are lattice/reciprocal-lattice vectors */
extern real R[3][3], G[3][3];
extern matrix3x3 Rm, Gm; /* same thing, but matrix3x3 */
extern real Vol; /* computational cell volume = |det Rm| */

/* index of current kpoint, for labeling output */
extern int kpoint_index;

/**************************************************************************/

extern void vector3_to_arr(real arr[3], vector3 v);
extern void matrix3x3_to_arr(real arr[3][3], matrix3x3 m);
extern scalar_complex cnumber2cscalar(cnumber c);
extern cnumber cscalar2cnumber(scalar_complex cs);
extern cvector3 cscalar32cvector3(const scalar_complex *cs);
extern void cvector32cscalar3(scalar_complex *cs, cvector3 v);

/**************************************************************************/

extern void init_epsilon(void);
extern const char *parity_string(maxwell_data *d);

#endif /* MPB_H */