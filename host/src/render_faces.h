#ifndef RLCD_HOST_RENDER_FACES_H
#define RLCD_HOST_RENDER_FACES_H

#include "u8g2.h"
#include "time_model.h"

/* Render the current face into the u8g2 buffer. Does NOT call sendBuffer. */
void render_face(u8g2_t *g, const clock_model_t *m);

#endif
