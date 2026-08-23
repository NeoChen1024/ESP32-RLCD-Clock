#include "render.h"

#include "model.h"
#include "render_faces.h"
#include "time_model.h"

/*
 * Face renderer: fill the platform model, then draw the shared single face.
 * The u8g2 draw calls are identical to the host simulator (render_faces.c is
 * compiled verbatim from host/src/), so the panel and host screenshots match.
 */

void render_frame(u8g2_t *g)
{
    clock_model_t m;
    time_model_now(&m);
    render_face(g, &m);
}
