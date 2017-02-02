/* gamma-randr.c -- X RANDR gamma adjustment source
   This file is part of Redshift.

   Redshift is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Redshift is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Redshift.  If not, see <http://www.gnu.org/licenses/>.

   Copyright (c) 2010  Jon Lund Steffensen <jonlst@gmail.com>

   Edited by Zoltan Padrah <zoltan_padrah@users.sf.net>:
   Removed Redshift specific parts
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(s) gettext(s)
#else
# define _(s) s
#endif

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "gamma_randr.h"
// #include "colorramp.h"


#define RANDR_VERSION_MAJOR  1
#define RANDR_VERSION_MINOR  3


int
randr_init(randr_state_t *state)
{
    /* Initialize state. */
    state->screen_num = -1;
    state->crtc_num = -1;

    state->crtc_count = 0;
    state->crtcs = NULL;

    xcb_generic_error_t *error;

    /* Open X server connection */
    state->conn = xcb_connect(NULL, &state->preferred_screen);

    /* Query RandR version */
    xcb_randr_query_version_cookie_t ver_cookie =
        xcb_randr_query_version(state->conn, RANDR_VERSION_MAJOR,
                                RANDR_VERSION_MINOR);
    xcb_randr_query_version_reply_t *ver_reply =
        xcb_randr_query_version_reply(state->conn, ver_cookie, &error);

    /* TODO What does it mean when both error and ver_reply is NULL?
       Apparently, we have to check both to avoid seg faults. */
    if (error || ver_reply == NULL) {
        int ec = (error != 0) ? error->error_code : -1;
        fprintf(stderr, _("`%s' returned error %d\n"),
                "RANDR Query Version", ec);
        xcb_disconnect(state->conn);
        return -1;
    }

    if (ver_reply->major_version != RANDR_VERSION_MAJOR ||
        ver_reply->minor_version < RANDR_VERSION_MINOR) {
        fprintf(stderr, _("Unsupported RANDR version (%u.%u)\n"),
                ver_reply->major_version, ver_reply->minor_version);
        free(ver_reply);
        xcb_disconnect(state->conn);
        return -1;
    }

    free(ver_reply);

    return 0;
}

int
randr_start(randr_state_t *state)
{
    xcb_generic_error_t *error;

    int screen_num = state->screen_num;
    if (screen_num < 0) screen_num = state->preferred_screen;

    /* Get screen */
    const xcb_setup_t *setup = xcb_get_setup(state->conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    state->screen = NULL;

    for (int i = 0; iter.rem > 0; i++) {
        if (i == screen_num) {
            state->screen = iter.data;
            break;
        }
        xcb_screen_next(&iter);
    }

    if (state->screen == NULL) {
        fprintf(stderr, _("Screen %i could not be found.\n"),
                screen_num);
        return -1;
    }

    /* Get list of CRTCs for the screen */
    xcb_randr_get_screen_resources_current_cookie_t res_cookie =
        xcb_randr_get_screen_resources_current(state->conn,
                                               state->screen->root);
    xcb_randr_get_screen_resources_current_reply_t *res_reply =
        xcb_randr_get_screen_resources_current_reply(state->conn,
                                                     res_cookie,
                                                     &error);

    if (error) {
        fprintf(stderr, _("`%s' returned error %d\n"),
                "RANDR Get Screen Resources Current",
                error->error_code);
        return -1;
    }

    state->crtc_count = res_reply->num_crtcs;
    state->crtcs = calloc(state->crtc_count, sizeof(randr_crtc_state_t));
    if (state->crtcs == NULL) {
        perror("malloc");
        state->crtc_count = 0;
        return -1;
    }

    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res_reply);

    /* Save CRTC identifier in state */
    for (unsigned int i = 0; i < state->crtc_count; i++) {
        state->crtcs[i].crtc = crtcs[i];
    }

    free(res_reply);

    /* Save size and gamma ramps of all CRTCs.
       Current gamma ramps are saved so we can restore them
       at program exit. */
    for (unsigned int i = 0; i < state->crtc_count; i++) {
        xcb_randr_crtc_t crtc = state->crtcs[i].crtc;

        /* Request size of gamma ramps */
        xcb_randr_get_crtc_gamma_size_cookie_t gamma_size_cookie =
            xcb_randr_get_crtc_gamma_size(state->conn, crtc);
        xcb_randr_get_crtc_gamma_size_reply_t *gamma_size_reply =
            xcb_randr_get_crtc_gamma_size_reply(state->conn,
                                                gamma_size_cookie,
                                                &error);

        if (error) {
            fprintf(stderr, _("`%s' returned error %d\n"),
                    "RANDR Get CRTC Gamma Size",
                    error->error_code);
            return -1;
        }

        unsigned int ramp_size = gamma_size_reply->size;
        state->crtcs[i].ramp_size = ramp_size;

        free(gamma_size_reply);

        if (ramp_size == 0) {
            fprintf(stderr, _("Gamma ramp size too small: %i\n"),
                    ramp_size);
            return -1;
        }

        /* Request current gamma ramps */
        xcb_randr_get_crtc_gamma_cookie_t gamma_get_cookie =
            xcb_randr_get_crtc_gamma(state->conn, crtc);
        xcb_randr_get_crtc_gamma_reply_t *gamma_get_reply =
            xcb_randr_get_crtc_gamma_reply(state->conn,
                                           gamma_get_cookie,
                                           &error);

        if (error) {
            fprintf(stderr, _("`%s' returned error %d\n"),
                    "RANDR Get CRTC Gamma", error->error_code);
            return -1;
        }

        uint16_t *gamma_r =
            xcb_randr_get_crtc_gamma_red(gamma_get_reply);
        uint16_t *gamma_g =
            xcb_randr_get_crtc_gamma_green(gamma_get_reply);
        uint16_t *gamma_b =
            xcb_randr_get_crtc_gamma_blue(gamma_get_reply);

        /* Allocate space for saved gamma ramps */
        state->crtcs[i].saved_ramps =
            malloc(3*ramp_size*sizeof(uint16_t));
        if (state->crtcs[i].saved_ramps == NULL) {
            perror("malloc");
            free(gamma_get_reply);
            return -1;
        }

        /* Copy gamma ramps into CRTC state */
        memcpy(&state->crtcs[i].saved_ramps[0*ramp_size], gamma_r,
               ramp_size*sizeof(uint16_t));
        memcpy(&state->crtcs[i].saved_ramps[1*ramp_size], gamma_g,
               ramp_size*sizeof(uint16_t));
        memcpy(&state->crtcs[i].saved_ramps[2*ramp_size], gamma_b,
               ramp_size*sizeof(uint16_t));

        free(gamma_get_reply);
    }

    return 0;
}

void
randr_restore(randr_state_t *state)
{
    xcb_generic_error_t *error;

    /* Restore CRTC gamma ramps */
    for (unsigned int i = 0; i < state->crtc_count; i++) {
        xcb_randr_crtc_t crtc = state->crtcs[i].crtc;

        unsigned int ramp_size = state->crtcs[i].ramp_size;
        uint16_t *gamma_r = &state->crtcs[i].saved_ramps[0*ramp_size];
        uint16_t *gamma_g = &state->crtcs[i].saved_ramps[1*ramp_size];
        uint16_t *gamma_b = &state->crtcs[i].saved_ramps[2*ramp_size];

        /* Set gamma ramps */
        xcb_void_cookie_t gamma_set_cookie =
            xcb_randr_set_crtc_gamma_checked(state->conn, crtc,
                                             ramp_size, gamma_r,
                                             gamma_g, gamma_b);
        error = xcb_request_check(state->conn, gamma_set_cookie);

        if (error) {
            fprintf(stderr, _("`%s' returned error %d\n"),
                    "RANDR Set CRTC Gamma", error->error_code);
            fprintf(stderr, _("Unable to restore CRTC %i\n"), i);
        }
    }
}

void
randr_free(randr_state_t *state)
{
    /* Free CRTC state */
    for (unsigned int i = 0; i < state->crtc_count; i++) {
        free(state->crtcs[i].saved_ramps);
    }
    free(state->crtcs);

    /* Close connection */
    xcb_disconnect(state->conn);
}

void
randr_print_help(FILE *f)
{
    fputs(_("Adjust gamma ramps with the X RANDR extension.\n"), f);
    fputs("\n", f);

    /* TRANSLATORS: RANDR help output
       left column must not be translated */
    fputs(_("  screen=N\tX screen to apply adjustments to\n"
            "  crtc=N\tCRTC to apply adjustments to\n"), f);
    fputs("\n", f);
}

int
randr_set_gamma_for_crtc(randr_state_t *state, int crtc_num,
                         unsigned int gamma_size,
                         const uint16_t *gamma_r, const uint16_t *gamma_g,
                         const uint16_t *gamma_b){
    xcb_generic_error_t *error;

    if (crtc_num >= (int)state->crtc_count || crtc_num < 0) {
        fprintf(stderr, _("CRTC %d does not exist. "),
                state->crtc_num);
        if (state->crtc_count > 1) {
            fprintf(stderr, _("Valid CRTCs are [0-%d].\n"),
                    state->crtc_count-1);
        } else {
            fprintf(stderr, _("Only CRTC 0 exists.\n"));
        }

        return -1;
    }

    xcb_randr_crtc_t crtc = state->crtcs[crtc_num].crtc;
    // unsigned int ramp_size = state->crtcs[crtc_num].ramp_size;

    /* Set new gamma ramps */
    xcb_void_cookie_t gamma_set_cookie =
        xcb_randr_set_crtc_gamma_checked(state->conn, crtc,
                                         gamma_size, gamma_r,
                                         gamma_g, gamma_b);
    error = xcb_request_check(state->conn, gamma_set_cookie);

    if (error) {
        fprintf(stderr, _("`%s' returned error %d\n"),
                "RANDR Set CRTC Gamma", error->error_code);
        return -1;
    }
    return 0;
}

int
randr_set_gamma(randr_state_t *state, unsigned int gamma_size,
                const uint16_t *gamma_r, const uint16_t *gamma_g,
                const uint16_t *gamma_b)
{
    int r;

    /* If no CRTC number has been specified,
       set temperature on all CRTCs. */
    if (state->crtc_num < 0) {
        for (unsigned int i = 0; i < state->crtc_count; i++) {
            r = randr_set_gamma_for_crtc(state, i,
                                         gamma_size, gamma_r, gamma_g, gamma_b);
            if (r < 0) return -1;
        }
    } else {
        return randr_set_gamma_for_crtc(state, state->crtc_num,
                                        gamma_size, gamma_r, gamma_g, gamma_b);
    }

    return 0;
}
