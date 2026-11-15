/* Integration-test probe: report the live X server's pointer and clients. */
#include <X11/Xlib.h>
#include <stdio.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window root = DefaultRootWindow(d), rr, parent, child, *children, focused;
    unsigned count, mask;
    int x, y, wx, wy, revert;
    XQueryPointer(d, root, &rr, &child, &x, &y, &wx, &wy, &mask);
    XGetInputFocus(d, &focused, &revert);
    printf("pointer=%d,%d focus=%lu\n", x, y, focused);
    if (!XQueryTree(d, root, &rr, &parent, &children, &count)) return 1;
    unsigned terminals = 0;
    for (unsigned i = 0; i < count; i++) {
        char *name = NULL;
        XWindowAttributes a;
        if (XGetWindowAttributes(d, children[i], &a) && a.map_state == IsViewable
            && !a.override_redirect && XFetchName(d, children[i], &name)) {
            printf("window=%lu geometry=%d,%d,%d,%d name=%s\n",
                   children[i], a.x, a.y, a.width, a.height, name);
            terminals++;
        }
        if (name) XFree(name);
    }
    printf("terminals=%u\n", terminals);
    XFree(children);
    XCloseDisplay(d);
    return 0;
}
