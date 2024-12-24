#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
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

static Display* disp;
static bool quit;
static int main_screen;
static Window rootwin;
static int screen_width;
static int screen_height;
static int curr_workspace;
static Client* workspace_first;
static Client* workspace_curr;
static void configurenotify(XEvent* e);
static void configurerequest(XEvent* e);
static void keypress(XEvent* e);
static void destroynotify(XEvent* e);
static void maprequest(XEvent* e);
static Workspace workspaces[WORKSPACE_COUNT];
static Cursor cursor;
static unsigned int focus_color;
static unsigned int unfocus_color;
static void spawn(const Arg arg);
static void add_window(Window w);

#define FOCUS   "rgb:bc/57/66"
#define UNFOCUS "rgb:88/88/88"
#define MOD     Mod1Mask

const char* dmenu_cmd[] = { "dmenu_run", NULL };

static Keybind keys[] = {
    { MOD | ShiftMask, XK_p, spawn, { .command = dmenu_cmd } },
};

static void (*events[LASTEvent])(XEvent* e) = {
    [KeyPress] = keypress,
    [DestroyNotify] = destroynotify,
    [MapRequest] = maprequest,
    [ConfigureNotify] = configurenotify,
    [ConfigureRequest] = configurerequest,
};

static void
tile_screen(void)
{
    // there are three different cases
    // 1. there are no windows -> do nothing
    // 2. there is a single window -> add space around the only window
    // 3. there are multiple windows -> count the amount of windows and divide the space evenly
    const int space = 10;
    if (workspace_first != NULL && workspace_first->next == NULL) {
        // special case there is only a single window
        XMoveResizeWindow(disp, workspace_first->window, space, space, screen_width - 3 * space, screen_height - 3 * space);
    } else if (workspace_first != NULL && workspace_first->next != NULL) { // multiple windows
        const int master_size = 0.55 * screen_width;
        XMoveResizeWindow(disp, workspace_first->window, space, space, master_size, screen_height - 2 * space);
        int x = master_size + 3 * space;
        int y = space;
        int tile_height = screen_width - master_size - 5 * space;
        int num_windows = 0;

        for (Client* cl = workspace_first->next; cl != NULL; cl = cl->next) {
            ++num_windows;
        }

        for (Client* cl = workspace_first->next; cl != NULL; cl = cl->next) {
            XMoveResizeWindow(disp, cl->window, x, y, tile_height, (screen_height / num_windows) - 2 * space);
            y += screen_height / num_windows;
        }
    }
}

static void
update_curr(void)
{
    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
        if (workspace_curr == cl) {
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
die(const char* e)
{
    fprintf(stdout, "stupid: %s\n", e);
    exit(1);
}

static void
save_state(int idx)
{
    workspaces[idx].curr = workspace_curr;
    workspaces[idx].first = workspace_first;
}

static void
update_global(int idx)
{
    workspace_first = workspaces[idx].first;
    workspace_curr = workspaces[idx].curr;
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

    if (workspace_first == NULL) {
        // there are no existing clients so update them
        cl->next = NULL;
        cl->prev = NULL;
        cl->window = w;
        workspace_first = cl;
    } else {
        Client* last;
        for (last = workspace_first; last->next != NULL; last = last->next)
            ;

        cl->next = NULL;
        cl->prev = last;
        cl->window = w;
        last->next = cl;
    }

    workspace_curr = cl;
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

    while (!quit && !XNextEvent(disp, &event)) {
        // handle events we know how to handle
        if (events[event.type]) {
            events[event.type](&event);
        }
    }
}

static void
go_workspace(int idx)
{
    // don't do anything if we're already in the correct workspace
    if (idx == curr_workspace)
        return;

    // since the workspaces differ we want to unmap each window that is not currently
    // in the workspace we're switching to. XUnmapWindow hides a given window untill it is
    // brought back using the XMapWindow function
    if (workspace_first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
            XUnmapWindow(disp, cl->window);
        }
    }

    // we need to save the state that the workspace is in such that when we switch back
    // between workspaces the position of the windows stays the same.
    save_state(curr_workspace);
    update_global(idx); // update the global state with the current workspace

    // map all of the windows that belong to the workspace that we switched to.
    if (workspace_first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
            XMapWindow(disp, cl->window);
        }
    }
}

static void
maprequest(XEvent* e)
{
    XMapRequestEvent* event = &e->xmaprequest;
    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
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
destroynotify(XEvent* e)
{
    XDestroyWindowEvent* dwe = &e->xdestroywindow;
    bool found_window = false;

    // ensure that the window actually exists. this might not be necessary but nice to check
    // that we don't do anything unexpected if the window isn't managed.
    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
        if (dwe->window == cl->window) {
            found_window = true;
            break;
        }
    }
    if (!found_window) {
        return;
    }

    // TODO: remove window
    // TODO: tile
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
    quit = false;

    cursor = XCreateFontCursor(disp, XC_left_ptr);
    XDefineCursor(disp, rootwin, cursor);

    focus_color = get_color(FOCUS);
    unfocus_color = get_color(UNFOCUS);

    setup_keybinds();

    for (int i = 0; i < WORKSPACE_COUNT; ++i) {
        workspaces[i].first = NULL;
        workspaces[i].curr = NULL;
    }

    // make xorg send window management events to us.
    XSelectInput(disp, rootwin, SubstructureNotifyMask | SubstructureRedirectMask);

    // start listening for XEvents
    start();

    XFreeCursor(disp, cursor);
    XCloseDisplay(disp);
    return 0;
}
