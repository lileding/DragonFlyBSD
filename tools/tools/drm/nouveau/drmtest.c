#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open card0"); return 1; }
    drmModeRes *r = drmModeGetResources(fd);
    if (!r) { printf("drmModeGetResources NULL (is it a modeset device?)\n"); return 1; }
    printf("resources: connectors=%d crtcs=%d encoders=%d planes?\n",
           r->count_connectors, r->count_crtcs, r->count_encoders);
    for (int i = 0; i < r->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, r->connectors[i]);
        if (!c) continue;
        printf("connector id=%u type=%u status=%d modes=%d\n",
               c->connector_id, c->connector_type, c->connection, c->count_modes);
        for (int m = 0; m < c->count_modes && m < 8; m++)
            printf("    %ux%u@%u %s\n", c->modes[m].hdisplay,
                   c->modes[m].vdisplay, c->modes[m].vrefresh, c->modes[m].name);
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(r);
    close(fd);
    return 0;
}
