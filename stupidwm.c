#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define WORKSPACE_COUNT 10

// this is a generic argument to some functions we can use this to make defining keybinds a lot easier.
// for example we want to give a workspace index or a command to a function.
typedef union {
    const char** command;
    const int workspace_idx;
} Arg;

typedef struct {
    unsigned int mod;
    KeySym ks;
    void (*function)(const Arg arg);
    const Arg arg;
} Keybind;

typedef struct Client {
    struct Client* next;
    struct Client* prev;
    Window window;
} Client;

typedef struct Workspace {
    Client* first;
    Client* curr;
} Workspace;

typedef struct Monitor {
    int x, y;
    int width, height;
    int screen;
    Window bar_window; // status bar window where the bar will be rendered
    GC graphics_ctx;   // graphics context for drawing the bar
    int curr_workspace;
    struct Monitor* next;
    bool primary;
} Monitor;

static void
die(const char* e)
{
    fprintf(stdout, "stupid: %s\n", e);
    exit(1);
}

#define SEL_MONITOR_WS (workspaces[selected_monitor->curr_workspace])

static Display* disp;
static bool quit_flag;
static int main_screen; // this is consistent between monitors
static Window rootwin;
static Workspace workspaces[WORKSPACE_COUNT]; // this is global between monitors
static Cursor cursor;
static unsigned int focus_color;
static unsigned int unfocus_color;
static XftFont* font;
static XftDraw* xft;
static XftColor xft_focus_color;
static XftColor xft_unfocus_color;

static void spawn(const Arg arg);
static void kill_curr();
static void add_window(Window w);
static void client_to_workspace(const Arg arg);
static void change_workspace(const Arg arg);
static void quit();

static void move_left();
static void move_up();
static void move_down();
static void move_right();

static Monitor* monitors;
static Monitor* selected_monitor;

// bar related stuff
static Window bar_window;
static GC graphics_ctx;
static int bar_height = 20;
static const char* tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };

// x events
static void configurenotify(XEvent* e);
static void configurerequest(XEvent* e);
static void keypress(XEvent* e);
static void destroynotify(XEvent* e);
static void maprequest(XEvent* e);
static void enternotify(XEvent* e);
static void expose(XEvent* e);

#define FOCUS   "#f9f5d7"
#define UNFOCUS "#282828"
#define MOD     Mod4Mask

const char* dmenu_cmd[] = { "dmenu_run", NULL };
const char* term_cmd[] = { "kitty", NULL };

#define DESKTOPCHANGE(K, N)                               \
    { MOD, K, change_workspace, { .workspace_idx = N } }, \
        { MOD | ShiftMask, K, client_to_workspace, { .workspace_idx = N } },

#define MOVEMENT(K, F) { MOD, K, F, { NULL } },

static Keybind keys[] = {
    { MOD | ShiftMask, XK_p, spawn, { .command = dmenu_cmd } },
    { MOD | ShiftMask, XK_q, kill_curr, { NULL } },
    { MOD | ShiftMask, XK_Return, spawn, { .command = term_cmd } },
    { MOD | ShiftMask, XK_e, quit, { NULL } },
    DESKTOPCHANGE(XK_1, 0)
        DESKTOPCHANGE(XK_2, 1)
            DESKTOPCHANGE(XK_3, 2)
                DESKTOPCHANGE(XK_4, 3)
                    DESKTOPCHANGE(XK_5, 4)
                        DESKTOPCHANGE(XK_6, 5)
                            DESKTOPCHANGE(XK_7, 6)
                                DESKTOPCHANGE(XK_8, 7)
                                    DESKTOPCHANGE(XK_9, 8)
                                        DESKTOPCHANGE(XK_0, 9)
                                            MOVEMENT(XK_h, move_left)
                                                MOVEMENT(XK_l, move_right)
                                                    MOVEMENT(XK_k, move_up)
                                                        MOVEMENT(XK_j, move_down)
};

#define FONT "Iosevka Comfy:size=13"

static void (*events[LASTEvent])(XEvent* e) = {
    [KeyPress] = keypress,
    [DestroyNotify] = destroynotify,
    [MapRequest] = maprequest,
    [ConfigureNotify] = configurenotify,
    [ConfigureRequest] = configurerequest,
    [EnterNotify] = enternotify,
    [Expose] = expose,
};

static void
setup_bar(void)
{
    XSetWindowAttributes wa = {
        .override_redirect = 1,
        .background_pixel = unfocus_color,
        .event_mask = ExposureMask,
    };

    bar_window = XCreateWindow(disp, rootwin,
        0, 0, selected_monitor->width, bar_height, 0,
        DefaultDepth(disp, main_screen),
        CopyFromParent, DefaultVisual(disp, main_screen),
        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

    font = XftFontOpenName(disp, main_screen, FONT);
    if (!font) {
        die("failed to load font");
    }

    xft = XftDrawCreate(disp, bar_window, XDefaultVisual(disp, main_screen), XDefaultColormap(disp, main_screen));
    if (!xft) {
        die("failed to create xft draw context");
    }

    XftColorAllocName(disp, XDefaultVisual(disp, main_screen),
        XDefaultColormap(disp, main_screen),
        FOCUS, &xft_focus_color);
    XftColorAllocName(disp, XDefaultVisual(disp, main_screen),
        XDefaultColormap(disp, main_screen),
        UNFOCUS, &xft_unfocus_color);

    graphics_ctx = XCreateGC(disp, bar_window, 0, NULL);
    XMapWindow(disp, bar_window);
}

static void
update_curr(void)
{
    for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
        if (SEL_MONITOR_WS.curr == cl) {
            XSetWindowBorderWidth(disp, cl->window, 5);
            XSetWindowBorder(disp, cl->window, focus_color);
            XSetInputFocus(disp, cl->window, RevertToParent, CurrentTime);
            XRaiseWindow(disp, cl->window);
        } else {
            XSetWindowBorder(disp, cl->window, unfocus_color);
        }
    }
}

static void
move_left(void)
{
    if (!SEL_MONITOR_WS.curr || !SEL_MONITOR_WS.first)
        return;

    SEL_MONITOR_WS.curr = SEL_MONITOR_WS.first;
    update_curr();
}

static void
move_right(void)
{
    if (!SEL_MONITOR_WS.curr || !SEL_MONITOR_WS.first)
        return;

    // If we're on master, move to the first stacked window
    if (SEL_MONITOR_WS.curr == SEL_MONITOR_WS.first && SEL_MONITOR_WS.first->next) {
        SEL_MONITOR_WS.curr = SEL_MONITOR_WS.first->next;
    }
    update_curr();
}

static void
move_up(void)
{
    if (!SEL_MONITOR_WS.curr || !SEL_MONITOR_WS.first)
        return;

    // If we're not on master and have a previous window
    if (SEL_MONITOR_WS.curr != SEL_MONITOR_WS.first && SEL_MONITOR_WS.curr->prev) {
        SEL_MONITOR_WS.curr = SEL_MONITOR_WS.curr->prev;
    }
    update_curr();
}

static void
move_down(void)
{
    if (!SEL_MONITOR_WS.curr || !SEL_MONITOR_WS.first)
        return;

    // If we have a next window
    if (SEL_MONITOR_WS.curr->next) {
        SEL_MONITOR_WS.curr = SEL_MONITOR_WS.curr->next;
    }
    update_curr();
}

static void
cleanup_font(void)
{
    if (xft) {
        XftDrawDestroy(xft);
        xft = NULL;
    }

    if (font) {
        XftFontClose(disp, font);
        font = NULL;
    }

    XftColorFree(disp, DefaultVisual(disp, main_screen),
        DefaultColormap(disp, main_screen), &xft_focus_color);
    XftColorFree(disp, DefaultVisual(disp, main_screen),
        DefaultColormap(disp, main_screen), &xft_unfocus_color);
}

static void
draw_bar(void)
{
    XSetForeground(disp, graphics_ctx, unfocus_color);
    XFillRectangle(disp, bar_window, graphics_ctx, 0, 0, selected_monitor->width, bar_height);

    int x = 0;
    int tag_width = 20;
    XGlyphInfo extents;

    for (int i = 0; i < WORKSPACE_COUNT; i++) {
        XftTextExtentsUtf8(disp, font, (XftChar8*)tags[i], strlen(tags[i]), &extents);
        tag_width = extents.xOff + 10;

        XSetForeground(disp, graphics_ctx, i == selected_monitor->curr_workspace ? focus_color : unfocus_color);
        XFillRectangle(disp, bar_window, graphics_ctx, x, 0, tag_width, bar_height);

        XftDrawStringUtf8(xft, i == selected_monitor->curr_workspace ? &xft_unfocus_color : &xft_focus_color,
            font, x + 5, bar_height - (bar_height - font->ascent) / 2,
            (XftChar8*)tags[i], strlen(tags[i]));

        x += tag_width;
    }
}

static void
expose(XEvent* e)
{
    XExposeEvent* ev = &e->xexpose;
    if (ev->window == bar_window && ev->count == 0) {
        draw_bar();
    }
}

static void
tile_screen(void)
{
    // there are three different cases
    // 1. there are no windows -> do nothing
    // 2. there is a single window -> add space around the only window
    // 3. there are multiple windows -> count the amount of windows and divide the space evenly
    const int space = 10;
    const int start_y = bar_height + space;
    if (SEL_MONITOR_WS.first != NULL && SEL_MONITOR_WS.first->next == NULL) {
        // special case there is only a single window
        XMoveResizeWindow(disp, SEL_MONITOR_WS.first->window, space, start_y, selected_monitor->width - 3 * space, selected_monitor->height - 3 * space);
    } else if (SEL_MONITOR_WS.first != NULL && SEL_MONITOR_WS.first->next != NULL) { // multiple windows
        const int master_size = 0.55 * selected_monitor->width;
        XMoveResizeWindow(disp, SEL_MONITOR_WS.first->window, space, start_y, master_size, selected_monitor->height - 2 * space);
        int x = master_size + 3 * space;
        int y = start_y;
        int tile_height = selected_monitor->width - master_size - 5 * space;
        int num_windows = 0;

        for (Client* cl = SEL_MONITOR_WS.first->next; cl != NULL; cl = cl->next) {
            ++num_windows;
        }

        for (Client* cl = SEL_MONITOR_WS.first->next; cl != NULL; cl = cl->next) {
            XMoveResizeWindow(disp, cl->window, x, y, tile_height, (selected_monitor->height / num_windows) - 2 * space);
            y += selected_monitor->height / num_windows;
        }
    }
}

static void
tile_monitor(Monitor* m)
{
    // there are three different cases
    // 1. there are no windows -> do nothing
    // 2. there is a single window -> add space around the only window
    // 3. there are multiple windows -> count the amount of windows and divide the space evenly
    const int space = 10;
    const int start_y = bar_height + space;
    Client* master = workspaces[m->curr_workspace].first;
    if (master != NULL && master->next == NULL) {
        XMoveResizeWindow(disp, SEL_MONITOR_WS.first->window, space, start_y, selected_monitor->width - 3 * space, selected_monitor->height - 3 * space);
    } else if (master != NULL && master->next != NULL) {
        const int master_size = 0.55 * m->width;
        XMoveResizeWindow(disp, master->window, m->x + space, m->y + start_y, master_size, m->height - 2 * space);
        int x = m->x + master_size + 3 * space;
        int y = m->y + start_y;
        int tile_width = m->width - master_size - 5 * space;
        int nwindows = 0;

        for (Client* cl = master->next; cl != NULL; cl = cl->next)
            ++nwindows;

        for (Client* cl = master->next; cl != NULL; cl = cl->next) {
            XMoveResizeWindow(disp, cl->window, x, y, tile_width, (selected_monitor->height / nwindows) - 2 * space);
            y += selected_monitor->height / nwindows;
        }
    }
}

static Monitor*
monitor_from_window(Window w)
{
    if (w == rootwin) {
        return selected_monitor;
    }

    int x, y;
    Window child;
    for (Monitor* m = monitors; m; m = m->next) {
        if (x >= m->x && x < m->x + m->width &&
            y >= m->y && y < m->y + m->height)
            return m;
    }

    return selected_monitor;
}

static void
focus_monitor(Monitor* m)
{
    if (m && m != selected_monitor) {
        selected_monitor = m;
        update_curr();
        draw_bar();
    }
}

static void
focus_next_monitor()
{
    if (!selected_monitor || !selected_monitor->next) {
        return;
    }

    focus_monitor(selected_monitor->next);
}

static void
save_state(int idx)
{
    workspaces[idx].curr = SEL_MONITOR_WS.curr;
    workspaces[idx].first = SEL_MONITOR_WS.first;
}

static void
update_global(int idx)
{
    SEL_MONITOR_WS.first = workspaces[idx].first;
    SEL_MONITOR_WS.curr = workspaces[idx].curr;
    selected_monitor->curr_workspace = idx;
}

// add window allocates a client and updates the global values
static void
add_window(Window w)
{
    Client* cl = calloc(1, sizeof(Client));
    if (cl == NULL) {
        die("failed calloc");
    }

    if (SEL_MONITOR_WS.first == NULL) {
        // there are no existing clients so update them
        cl->next = NULL;
        cl->prev = NULL;
        cl->window = w;
        SEL_MONITOR_WS.first = cl;
    } else {
        Client* last;
        for (last = SEL_MONITOR_WS.first; last->next != NULL; last = last->next)
            ;

        cl->next = NULL;
        cl->prev = last;
        cl->window = w;
        last->next = cl;
    }

    // subscribe to events when the mouse moves to this window such that we can
    // change the current window
    XSelectInput(disp, w, EnterWindowMask);

    SEL_MONITOR_WS.curr = cl;
}

unsigned long
get_color(const char* color)
{
    XColor c;
    Colormap map = DefaultColormap(disp, main_screen);

    if (!XAllocNamedColor(disp, map, color, &c, &c)) {
        die("error parsing color");
    }

    return c.pixel;
}

static void
sigchld(int unused)
{
    // setup signal handling for child processes
    if (signal(SIGCHLD, sigchld) == SIG_ERR) {
        die("sigchld handler failed");
    }
    while (0 < waitpid(-1, NULL, WNOHANG))
        ;
}

static void
start(void)
{
    XEvent event;

    while (!quit_flag && !XNextEvent(disp, &event)) {
        // handle events we know how to handle
        if (events[event.type]) {
            events[event.type](&event);
        }
    }
}

static void
remove_window(Window w)
{
    for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
        if (cl->window == w) {
            if (cl->prev == NULL && cl->next == NULL) {
                free(SEL_MONITOR_WS.first);
                SEL_MONITOR_WS.first = NULL;
                SEL_MONITOR_WS.curr = NULL;
                return;
            }

            if (cl->prev == NULL) {
                SEL_MONITOR_WS.first = cl->next;
                cl->next->prev = NULL;
                SEL_MONITOR_WS.curr = cl->next;
            } else if (cl->next == NULL) {
                cl->prev->next = NULL;
                cl->next->prev = cl->prev;
                SEL_MONITOR_WS.curr = cl->prev;
            } else {
                cl->prev->next = cl->next;
                cl->next->prev = cl->prev;
                SEL_MONITOR_WS.curr = cl->prev;
            }
        }
    }
}

static void
client_to_workspace(const Arg arg)
{
    Client* tmp = SEL_MONITOR_WS.curr;
    int tmp2 = arg.workspace_idx;

    if (arg.workspace_idx == selected_monitor->curr_workspace || SEL_MONITOR_WS.curr == NULL)
        return;

    update_global(arg.workspace_idx);
    add_window(tmp->window);
    save_state(arg.workspace_idx);

    update_global(tmp2);
    remove_window(SEL_MONITOR_WS.curr->window);

    tile_screen();
    update_curr();
}

static void
change_workspace(const Arg arg)
{
    // don't do anything if we're already in the correct workspace
    if (arg.workspace_idx == selected_monitor->curr_workspace)
        return;

    // since the workspaces differ we want to unmap each window that is not currently
    // in the workspace we're switching to. XUnmapWindow hides a given window untill it is
    // brought back using the XMapWindow function
    if (SEL_MONITOR_WS.first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
            XUnmapWindow(disp, cl->window);
        }
    }

    // we need to save the state that the workspace is in such that when we switch back
    // between workspaces the position of the windows stays the same.
    save_state(selected_monitor->curr_workspace);
    update_global(arg.workspace_idx); // update the global state with the current workspace

    // map all of the windows that belong to the workspace that we switched to.
    if (SEL_MONITOR_WS.first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
            XMapWindow(disp, cl->window);
        }
    }

    tile_screen();
    update_curr();
    draw_bar();
}

static void
maprequest(XEvent* e)
{
    XMapRequestEvent* event = &e->xmaprequest;
    for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
        if (event->window == cl->window) {
            XMapWindow(disp, event->window);
            return;
        }
    }

    add_window(event->window);
    XMapWindow(disp, event->window);
    tile_screen();
    update_curr();
}

static void
spawn(const Arg arg)
{
    if (fork() == 0) {
        if (fork() == 0) {
            if (disp) {
                close(ConnectionNumber(disp));
            }
            setsid();
            execvp((char*)arg.command[0], (char**)arg.command);
        }
        exit(0);
    }
}

static void
configurenotify(XEvent* e)
{
}

static void
configurerequest(XEvent* e)
{
    // handle window conf requests i.e. resize of move from different applications
    // here we just do what the request wants since we will be changing the state
    // later to be tiled (or follow whatever window composition schema)
    XConfigureRequestEvent* ev = &e->xconfigurerequest;
    XWindowChanges wc;
    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    XConfigureWindow(disp, ev->window, ev->value_mask, &wc);
}

static void
keypress(XEvent* e)
{
    XKeyEvent ev = e->xkey;
    int ks_ret;
    KeySym* ks = XGetKeyboardMapping(disp, ev.keycode, 1, &ks_ret);

    for (int i = 0; i < (sizeof(keys) / sizeof(*keys)); ++i) {
        if (keys[i].ks == *ks && keys[i].mod == ev.state) {
            keys[i].function(keys[i].arg);
        }
    }
}

static void
enternotify(XEvent* e)
{
    XEnterWindowEvent* ev = &e->xcrossing;

    // when the mouse hovers over the background we don't want to do anything
    if (ev->window == rootwin) {
        return;
    }

    for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
        if (cl->window == ev->window) {
            SEL_MONITOR_WS.curr = cl;
            update_curr();
            break;
        }
    }
}

static void
destroynotify(XEvent* e)
{
    XDestroyWindowEvent* dwe = &e->xdestroywindow;
    bool found_window = false;

    // ensure that the window actually exists. this might not be necessary but nice to check
    // that we don't do anything unexpected if the window isn't managed.
    for (Client* cl = SEL_MONITOR_WS.first; cl != NULL; cl = cl->next) {
        if (dwe->window == cl->window) {
            found_window = true;
            break;
        }
    }
    if (!found_window) {
        return;
    }

    remove_window(dwe->window);
    tile_screen();
    update_curr();
}

static void
send_kill_signal(Window w)
{
    XEvent ke;
    ke.type = ClientMessage;
    ke.xclient.window = w;
    ke.xclient.message_type = XInternAtom(disp, "WM_PROTOCOLS", 1);
    ke.xclient.format = 32;
    ke.xclient.data.l[0] = XInternAtom(disp, "WM_DELETE_WINDOW", 1);
    ke.xclient.data.l[1] = CurrentTime;
    XSendEvent(disp, w, False, NoEventMask, &ke);
}

static void
kill_curr(void)
{
    if (SEL_MONITOR_WS.curr != NULL) {
        XEvent ke;
        ke.type = ClientMessage;
        ke.xclient.window = SEL_MONITOR_WS.curr->window;
        ke.xclient.message_type = XInternAtom(disp, "WM_PROTOCOLS", True);
        ke.xclient.format = 32;
        ke.xclient.data.l[0] = XInternAtom(disp, "WM_DELETE_WINDOW", True);
        ke.xclient.data.l[1] = CurrentTime;
        XSendEvent(disp, SEL_MONITOR_WS.curr->window, 0, NoEventMask, &ke);
        send_kill_signal(SEL_MONITOR_WS.curr->window);
    }
}

static void
setup_keybinds(void)
{
    for (int i = 0; i < (sizeof(keys) / sizeof(*keys)); ++i) {
        KeySym ks = XKeysymToKeycode(disp, keys[i].ks);
        if (ks) {
            XGrabKey(disp, ks, keys[i].mod, rootwin, 1, GrabModeAsync, GrabModeAsync);
        }
    }
}

static void
quit()
{
    if (quit_flag) {
        cleanup_font();
        XUngrabKey(disp, AnyKey, AnyModifier, rootwin);
        XDestroySubwindows(disp, rootwin);
        fprintf(stdout, "stupidwm: quitting...");
        XCloseDisplay(disp);
        die("shutdown");
    }

    Window root_return, parent;
    Window* children;
    int i;
    unsigned int nchildren;
    XEvent ev;
    quit_flag = 1;
    XQueryTree(disp, rootwin, &root_return, &parent, &children, &nchildren);
    for (i = 0; i < nchildren; i++) {
        send_kill_signal(children[i]);
    }

    while (nchildren > 0) {
        XQueryTree(disp, rootwin, &root_return, &parent, &children, &nchildren);
        XNextEvent(disp, &ev);
        if (events[ev.type])
            events[ev.type](&ev);
    }

    XUngrabKey(disp, AnyKey, AnyModifier, rootwin);
    fprintf(stdout, "stupidwm: quitting\n");
}

static void
swap_curr_with_master()
{
    if (SEL_MONITOR_WS.first != NULL && SEL_MONITOR_WS.curr != SEL_MONITOR_WS.first && SEL_MONITOR_WS.curr != NULL) {
        Window tmp = SEL_MONITOR_WS.first->window;
        SEL_MONITOR_WS.first->window = SEL_MONITOR_WS.curr->window;
        SEL_MONITOR_WS.curr->window = tmp;
        SEL_MONITOR_WS.curr = SEL_MONITOR_WS.first;

        tile_screen();
        update_curr();
    }
}

static Monitor*
create_monitor(int x, int y, int width, int height, bool primary)
{
    Monitor* m = calloc(1, sizeof(Monitor));
    if (!m) {
        die("failed to allocate monitor");
    }

    // init properties
    m->x = x;
    m->y = y;
    m->width = width;
    m->height = height;
    m->primary = primary;
    m->curr_workspace = 0;
    m->next = NULL;

    XSetWindowAttributes wa = {
        .override_redirect = 1,
        .background_pixel = unfocus_color,
        .event_mask = ExposureMask,
    };

    m->bar_window = XCreateWindow(disp, rootwin,
        x, y, width, bar_height, 0,
        DefaultDepth(disp, main_screen),
        CopyFromParent, DefaultVisual(disp, main_screen),
        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

    m->graphics_ctx = XCreateGC(disp, m->bar_window, 0, NULL);
    XMapWindow(disp, m->bar_window);
    return m;
}

static void
setup_monitors(void)
{

    monitors = NULL;
    selected_monitor = NULL;

    int num_monitors;
    XRRScreenResources* res = XRRGetScreenResources(disp, rootwin);
    XRROutputInfo* output_info;
    XRRCrtcInfo* crtc_info;

    for (int i = 0; i < res->noutput; i++) {
        output_info = XRRGetOutputInfo(disp, res, res->outputs[i]);

        if (output_info->connection == RR_Connected && output_info->crtc) {
            crtc_info = XRRGetCrtcInfo(disp, res, output_info->crtc);

            Monitor* m = create_monitor(
                crtc_info->x,
                crtc_info->y,
                crtc_info->width,
                crtc_info->height,
                i == 0);

            if (!monitors) {
                monitors = m;
                selected_monitor = m;
            } else {
                Monitor* last;
                for (last = monitors; last->next; last = last->next)
                    ;
                last->next = m;
            }

            XRRFreeCrtcInfo(crtc_info);
        }
        XRRFreeOutputInfo(output_info);
    }

    XRRFreeScreenResources(res);

    if (!monitors) {
        monitors = create_monitor(0, 0, selected_monitor->width, selected_monitor->height, true);
        selected_monitor = monitors;
    }
}

int
main(int argc, char* argv[])
{
    disp = XOpenDisplay(NULL);
    if (disp == NULL) {
        die("cannot open display");
    }

    // setup signal for child processes
    sigchld(0);

    main_screen = XDefaultScreen(disp);
    rootwin = XRootWindow(disp, main_screen);

    quit_flag = false;

    cursor = XCreateFontCursor(disp, XC_left_ptr);
    XDefineCursor(disp, rootwin, cursor);

    focus_color = get_color(FOCUS);
    unfocus_color = get_color(UNFOCUS);

    setup_monitors();
    setup_keybinds();
    setup_bar();

    for (int i = 0; i < WORKSPACE_COUNT; ++i) {
        workspaces[i].first = NULL;
        workspaces[i].curr = NULL;
    }

    // make xorg send window management events to us.
    XSelectInput(disp, rootwin, SubstructureNotifyMask | SubstructureRedirectMask);

    draw_bar();

    // start listening for XEvents
    start();

    XFreeCursor(disp, cursor);
    XCloseDisplay(disp);
    return 0;
}
