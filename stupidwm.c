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
static Workspace workspaces[WORKSPACE_COUNT];
static Cursor cursor;
static unsigned int focus_color;
static unsigned int unfocus_color;
static void spawn(const Arg arg);
static void kill_curr();
static void add_window(Window w);
static void client_to_workspace(const Arg arg);
static void change_workspace(const Arg arg);

// x events
static void configurenotify(XEvent* e);
static void configurerequest(XEvent* e);
static void keypress(XEvent* e);
static void destroynotify(XEvent* e);
static void maprequest(XEvent* e);
static void enternotify(XEvent* e);

#define FOCUS   "rgb:bc/57/66"
#define UNFOCUS "rgb:88/88/88"
#define MOD     Mod1Mask

const char* dmenu_cmd[] = { "dmenu_run", NULL };
const char* term_cmd[] = { "kitty", NULL };

#define DESKTOPCHANGE(K, N)                               \
    { MOD, K, change_workspace, { .workspace_idx = N } }, \
        { MOD | ShiftMask, K, client_to_workspace, { .workspace_idx = N } },

static Keybind keys[] = {
    { MOD | ShiftMask, XK_p, spawn, { .command = dmenu_cmd } },
    { MOD | ShiftMask, XK_l, kill_curr, { NULL } },
    DESKTOPCHANGE(XK_0, 0)
        DESKTOPCHANGE(XK_1, 1)
            DESKTOPCHANGE(XK_2, 2)
                DESKTOPCHANGE(XK_3, 3)
                    DESKTOPCHANGE(XK_4, 4)
                        DESKTOPCHANGE(XK_5, 5)
                            DESKTOPCHANGE(XK_6, 6)
                                DESKTOPCHANGE(XK_7, 7)
                                    DESKTOPCHANGE(XK_8, 8)
                                        DESKTOPCHANGE(XK_9, 9)
};

static void (*events[LASTEvent])(XEvent* e) = {
    [KeyPress] = keypress,
    [DestroyNotify] = destroynotify,
    [MapRequest] = maprequest,
    [ConfigureNotify] = configurenotify,
    [ConfigureRequest] = configurerequest,
    [EnterNotify] = enternotify,
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

    // subscribe to events when the mouse moves to this window such that we can
    // change the current window
    XSelectInput(disp, w, EnterWindowMask);

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
remove_window(Window w)
{
    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
        if (cl->window == w) {
            if (cl->prev == NULL && cl->next == NULL) {
                free(workspace_first);
                workspace_first = NULL;
                workspace_curr = NULL;
                return;
            }

            if (cl->prev == NULL) {
                workspace_first = cl->next;
                cl->next->prev = NULL;
                workspace_curr = cl->next;
            } else if (cl->next == NULL) {
                cl->prev->next = NULL;
                cl->next->prev = cl->prev;
                workspace_curr = cl->prev;
            } else {
                cl->prev->next = cl->next;
                cl->next->prev = cl->prev;
                workspace_curr = cl->prev;
            }
        }
    }
}

static void
client_to_workspace(const Arg arg)
{
    Client* tmp = workspace_curr;
    int tmp2 = arg.workspace_idx;

    if (arg.workspace_idx == curr_workspace || workspace_curr == NULL)
        return;

    update_global(arg.workspace_idx);
    add_window(tmp->window);
    save_state(arg.workspace_idx);

    update_global(tmp2);
    remove_window(workspace_curr->window);

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
    if (workspace_first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
            XUnmapWindow(disp, cl->window);
        }
    }

    // we need to save the state that the workspace is in such that when we switch back
    // between workspaces the position of the windows stays the same.
    save_state(curr_workspace);
    update_global(arg.workspace_idx); // update the global state with the current workspace

    // map all of the windows that belong to the workspace that we switched to.
    if (workspace_first != NULL) {
        // we have windows that we need to unwrap
        for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
            XMapWindow(disp, cl->window);
        }
    }

    tile_screen();
    update_curr();
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
enternotify(XEvent* e)
{
    XEnterWindowEvent* ev = &e->xcrossing;

    // when the mouse hovers over the background we don't want to do anything
    if (ev->window == rootwin) {
        return;
    }

    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
        if (cl->window == ev->window) {
            workspace_curr = cl;
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
    for (Client* cl = workspace_first; cl != NULL; cl = cl->next) {
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
    if (workspace_curr != NULL) {
        XEvent ke;
        ke.type = ClientMessage;
        ke.xclient.window = workspace_curr->window;
        ke.xclient.message_type = XInternAtom(disp, "WM_PROTOCOLS", True);
        ke.xclient.format = 32;
        ke.xclient.data.l[0] = XInternAtom(disp, "WM_DELETE_WINDOW", True);
        ke.xclient.data.l[1] = CurrentTime;
        XSendEvent(disp, workspace_curr->window, 0, NoEventMask, &ke);
        send_kill_signal(workspace_curr->window);
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
