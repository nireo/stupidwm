#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

#define WORKSPACE_COUNT 10

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

static void (*events[LASTEvent])(XEvent* e) = {
    [KeyPress] = keypress,
    [DestroyNotify] = destroynotify,
    [MapRequest] = maprequest,
    [ConfigureNotify] = configurenotify,
    [ConfigureRequest] = configurerequest,
};

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

    //  TODO: add window
    XMapWindow(disp, event->window);
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
    // TODO: special keybindings
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
