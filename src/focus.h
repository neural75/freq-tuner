#ifndef FT_FOCUS_H
#define FT_FOCUS_H

/* Tracks the WM_CLASS of the focused window, fed by the GNOME Shell
 * extension over a UNIX DGRAM socket. The extension (or a helper) sends the
 * focused window's class as a datagram whenever focus changes; the daemon
 * just listens. No polling: if no datagram arrives, the last known class is
 * kept.
 */
struct ft_focus;

struct ft_focus *ft_focus_init(const char *sock_path, int verbose);
int ft_focus_fd(struct ft_focus *f);              /* -1 if no socket */
void ft_focus_handle(struct ft_focus *f);         /* drain pending datagrams */
const char *ft_focus_class(struct ft_focus *f);   /* may be empty */
void ft_focus_free(struct ft_focus *f);

#endif /* FT_FOCUS_H */