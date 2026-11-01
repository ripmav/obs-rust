/* Placeholders for modules not (yet) linked: report the command as unknown. */
#include "linuxinputs_api.h"
bool li_xcomp_cmd(struct script_line *sl) { (void)sl; return false; }
bool li_oss_cmd(struct script_line *sl) { (void)sl; return false; }
bool li_sndio_cmd(struct script_line *sl) { (void)sl; return false; }
bool li_jack_cmd(struct script_line *sl) { (void)sl; return false; }
bool li_v4l2out_cmd(struct script_line *sl) { (void)sl; return false; }
bool li_udev_cmd(struct script_line *sl) { (void)sl; return false; }
