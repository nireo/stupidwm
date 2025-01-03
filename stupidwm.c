#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
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
    Window bar_window; // status bar window where the bar will be rendered
    GC graphics_ctx;   // graphics context for drawing the bar
    Workspace workspaces[WORKSPACE_COUNT];
    int curr_workspace;
    struct Monitor* next;
    bool primary;
} Monitor;

static Display* disp;
static bool quit_flag;
static int main_screen;
static Window rootwin;
static int screen_width;
static int screen_height;
static int curr_workspace;
static Client* master_client;
static Client* selected_client;
static Workspace workspaces[WORKSPACE_COUNT];
static Cursor cursor;
static unsigned int focus_color;
static unsigned int unfocus_color;
static void spawn(const Arg arg);
static void kill_curr();
static void add_window(Window w);
static void client_to_workspace(const Arg arg);
static void change_workspace(const Arg arg);
static void quit();

static Monitor* monitors;
static Monitor* selected_monitor;

// bar related stuff
static Window bar_window;
static GC graphics_ctx;
static int bar_height = 20;
static const char* tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };

// x events
static void
configurenotify(XEvent* e);
static void configurerequest(XEvent* e);
static void keypress(XEvent* e);
static void destroynotify(XEvent* e);
static void maprequest(XEvent* e);
static void enternotify(XEvent* e);
static void expose(XEvent* e);

#define FOCUS   "rgb:bc/57/66"
#define UNFOCUS "rgb:88/88/88"
#define MOD     Mod4Mask

const char* dmenu_cmd[] = { "dmenu_run", NULL };
const char* term_cmd[] = { "kitty", NULL };

#define DESKTOPCHANGE(K, N)                               \
    { MOD, K, change_workspace, { .workspace_idx = N } }, \
        { MOD | ShiftMask, K, client_to_workspace, { .workspace_idx = N } },

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
};

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
        0, 0, screen_width, bar_height, 0,
        DefaultDepth(disp, main_screen),
        CopyFromParent, DefaultVisual(disp, main_screen),
        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

    graphics_ctx = XCreateGC(disp, bar_window, 0, NULL);
    XMapWindow(disp, bar_window);
}

static void
draw_bar(void)
{
    XSetForeground(disp, graphics_ctx, unfocus_color);
    XFillRectangle(disp, bar_window, graphics_ctx, 0, 0, screen_width, bar_height);

    int x = 0;
    int tag_width = 20;

    for (int i = 0; i < WORKSPACE_COUNT; i++) {
        XSetForeground(disp, graphics_ctx, i == curr_workspace ? focus_color : unfocus_color);
        XFillRectangle(disp, bar_window, graphics_ctx, x, 0, tag_width, bar_height);

        XSetForeground(disp, graphics_ctx, i == curr_workspace ? unfocus_color : focus_color);
        XDrawString(disp, bar_window, graphics_ctx, x + 5, bar_height - 5, tags[i], strlen(tags[i]));

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
    if (master_client != NULL && master_client->next == NULL) {
        // special case there is only a single window
        XMoveResizeWindow(disp, master_client->window, space, start_y, screen_width - 3 * space, screen_height - 3 * space);
    } else if (master_client != NULL && master_client->next != NULL) { // multiple windows
        const int master_size = 0.55 * screen_width;
        XMoveResizeWindow(disp, master_client->window, space, start_y, master_size, screen_height - 2 * space);
        int x = master_size + 3 * space;
        int y = start_y;
        int tile_height = screen_width - master_size - 5 * space;
        int num_windows = 0;

        for (Client* cl = master_client->next; cl != NULL; cl = cl->next) {
            ++num_windows;
        }

        for (Client* cl = master_client->next; cl != NULL; cl = cl->next) {
            XMoveResizeWindow(disp, cl->window, x, y, tile_height, (screen_height / num_windows) - 2 * space);
            y += screen_height / num_windows;
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
    Client* master = m->workspaces[m->curr_workspace].first;
    if (master != NULL && master->next == NULL) {
        XMoveResizeWindow(disp, master_client->window, space, start_y, screen_width - 3 * space, screen_height - 3 * space);
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
            XMoveResizeWindow(disp, cl->window, x, y, tile_width, (screen_height / nwindows) - 2 * space);
            y += screen_height / nwindows;
        }
    }
}

static void
update_curr(void)
{
    for (Client* cl = master_client; cl != NULL; cl = cl->next) {
        if (selected_client == cl) {
            XSetWindowBorderWidth(disp, cl->window, 5);
            XSetWindowBorder(disp, cl->window, focus_color);
            XSetInputFocus(disp, cl->window, RevertToParent, CurrentTime);
            XRaiseWindow(disp, cl->window);
        } else {
            XSetWindowBorder(disp, cl->window, unfocus_color);
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
die(const char* e)
{
    fprintf(stdout, "stupid: %s\n", e);
    exit(1);
}

static void
save_state(int idx)
{
    workspaces[idx].curr = selected_client;
    workspaces[idx].first = master_client;
}

static void
update_global(int idx)
{
    master_client = workspaces[idx].first;
    selected_client = workspaces[idx].curr;
    curr_workspace = idx;
}

// add window allocates a client and updates the global values
static void
add_window(Window w)
{
    Client* cl = calloc(1, sizeof(Client));
    if (cl == NULL) {
        die("failed calloc");
    }

    if (master_client == NULL) {
        // there are no existing clients so update them
        cl->next = NULL;
        cl->prev = NULL;
        cl->window = w;
        master_client = cl;
    } else {
        Client* last;
        for (last = master_client; last->next != NULL; last = last->next)
            ;

        cl->next = NULL;
        cl->prev = last;
        cl->window = w;
        last->next = cl;
    }

    // subscribe to events when the mouse moves to this window such that we can
    // change the current window
    XSelectInput(disp, w, EnterWindowMask);

    selected_client = cl;
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
    for (Client* cl = master_client; cl != NULL; cl = cl->next) {
        if (cl->window == w) {
            if (cl->prev == NULL && cl->next == NULL) {
                free(master_client);
                master_client = NULL;
                selected_client = NULL;
                return;
            }

            if (cl->prev == NULL) {
                master_client = cl->next;
                cl->next->prev = NULL;
                selected_client = cl->next;
            } else if (cl->next == NULL) {
                cl->prev->next = NULL;
                cl->next->prev = cl->prev;
                selected_client = cl->prev;
            } else {
                cl->prev->next = cl->next;
                cl->next->prev = cl->prev;
                selected_client = cl->prev;
            }
        }
    }
}

static void
client_to_workspace(const Arg arg)
{
    Client* tmp = selected_client;
    int tmp2 = arg.workspace_idx;

    if (arg.workspace_idx == curr_workspace || selected_client == NULL)
        return;

    update_global(arg.workspace_idx);
    add_window(tmp->window);
    save_state(arg.workspace_idx);

    update_global(tmp2);
    remove_window(selected_client->window);

    tile_screen();
    update_curr();
}

static void
change_workspace(const Arg arg)
{
    // don't do anything if we're already in the correct workspace
    if (arg.workspace_idx == curr_workspace)
        return;

    // since the workspaces differ we want to unmap each window that is not currently
    // in the workspace we're switching to. XUnmapWindow hides a given window untill it is
    // brought back using the XMapWindow function
    if (master_client != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = master_client; cl != NULL; cl = cl->next) {
            XUnmapWindow(disp, cl->window);
        }
    }

    // we need to save the state that the workspace is in such that when we switch back
    // between workspaces the position of the windows stays the same.
    save_state(curr_workspace);
    update_global(arg.workspace_idx); // update the global state with the current workspace

    // map all of the windows that belong to the workspace that we switched to.
    if (master_client != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = master_client; cl != NULL; cl = cl->next) {
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
    for (Client* cl = master_client; cl != NULL; cl = cl->next) {
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

    for (Client* cl = master_client; cl != NULL; cl = cl->next) {
        if (cl->window == ev->window) {
            selected_client = cl;
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
    for (Client* cl = master_client; cl != NULL; cl = cl->next) {
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
    if (selected_client != NULL) {
        XEvent ke;
        ke.type = ClientMessage;
        ke.xclient.window = selected_client->window;
        ke.xclient.message_type = XInternAtom(disp, "WM_PROTOCOLS", True);
        ke.xclient.format = 32;
        ke.xclient.data.l[0] = XInternAtom(disp, "WM_DELETE_WINDOW", True);
        ke.xclient.data.l[1] = CurrentTime;
        XSendEvent(disp, selected_client->window, 0, NoEventMask, &ke);
        send_kill_signal(selected_client->window);
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
    if (master_client != NULL && selected_client != master_client && selected_client != NULL) {
        Window tmp = master_client->window;
        master_client->window = selected_client->window;
        selected_client->window = tmp;
        selected_client = master_client;

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

    // init workspaces
    for (int i = 0; i < WORKSPACE_COUNT; ++i) {
        m->workspaces[i].first = NULL;
        m->workspaces[i].curr = NULL;
    }

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
        monitors = create_monitor(0, 0, screen_width, screen_height, true);
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

    screen_height = XDisplayHeight(disp, main_screen);
    screen_width = XDisplayWidth(disp, main_screen);
    quit_flag = false;

    cursor = XCreateFontCursor(disp, XC_left_ptr);
    XDefineCursor(disp, rootwin, cursor);

    focus_color = get_color(FOCUS);
    unfocus_color = get_color(UNFOCUS);

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
