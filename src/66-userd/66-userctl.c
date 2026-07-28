/*
 * 66-userctl.c
 *
 * Copyright (c) 2026 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 */

#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/types.h>

#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/stream.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66/info.h>
#include <66/shutdown.h>

#include "constants.h"
#include "userd.h"
#include "session.h"
#include "state.h"
#include "power.h"
#include "proto.h"

#define PAYLOAD_MAX 40

static wchar_t const field_suffix[] = L" :" ;

typedef struct urow_s urow_t ;
struct urow_s {
    char uid[U64_FMT] ;
    char const *state ;
    char name[USER_NAME_MAX] ;
    char nsessions[U32_FMT] ;
} ;

typedef struct srow_s srow_t ;
struct srow_s {
    char id[U64_FMT] ;
    char const *state ;
    char uid[UID_FMT] ;
    char user[USER_NAME_MAX] ;
    char seat[SESSION_FIELD_MAX] ;
    char where[SESSION_FIELD_MAX] ;
} ;

typedef struct ctx_s ctx_t ;
struct ctx_s {
    char const *statedir ;
    unsigned int icolor ;
    int force ;
} ;

static inline void put(char const *s)
{
    ostream_puts(ostream_1, s) ;
}

static inline void length_from_wchar(char const *s, size_t width)
{
    for (size_t l = info_length_from_wchar(s) ; l < width ; l++)
        ostream_put(ostream_1, " ", 1) ;
}

static inline void put_padded(char const *s, size_t width)
{
    ostream_puts(ostream_1, s) ;
    length_from_wchar(s, width) ;
}

static inline void put_label(char const *s)
{
    ostream_puts(ostream_1, log_color->info) ;
    ostream_puts(ostream_1, s) ;
    ostream_puts(ostream_1, log_color->off) ;
}

static inline void put_header(char const *s, size_t width)
{
    put_label(s) ;
    length_from_wchar(s, width) ;
}

static inline void put_nl(void)
{
    ostream_put(ostream_1, "\n", 1) ;
}

static inline void put_field(char const *label, char const *value)
{
    info_display_field_name(label) ;
    put(value && value[0] ? value : "-") ;
    put_nl() ;
}

static inline void put_field_u64(char const *label, uint64_t value)
{
    char b[U64_FMT] ;
    b[u64_fmt(b, value)] = 0 ;
    put_field(label, b) ;
}

// Numeric order of two decimal id strings, for sbl_sort_cmp.
static int idname_cmp(char const *a, char const *b)
{
    uint64_t x = 0, y = 0 ;
    u64_scan(a, &x) ;
    u64_scan(b, &y) ;
    return x < y ? -1 : x > y ? 1 : 0 ;
}

static int collect_ids(char const *dir, strbuf *ids)
{
    DIR *d = opendir(dir) ;
    if (!d)
        return errno == ENOENT ? 1 : 0 ; // no dir yet -> empty list, not an error

    struct dirent *e ;

    while ((e = readdir(d))) {

        if (e->d_name[0] == '.')
            continue ;

        uint64_t v ;
        size_t l = u64_scan(e->d_name, &v) ;

        if (!l || e->d_name[l])
            continue ; // not a pure decimal name

        if (!sbl_add(ids, e->d_name)) {
            closedir(d) ;
            return 0 ;
        }
    }

    closedir(d) ;

    // sbl_sort_cmp reports an empty list as 0 without touching errno.
    if (ids->len && !sbl_sort_cmp(ids, idname_cmp))
        return 0 ;

    return 1 ;
}

static void collect_ids_or_die(char const *statedir, char const *sub, strbuf *ids)
{
    char dir[strlen(statedir) + strlen(sub) + 1] ;
    auto_strings(dir, statedir, sub) ;

    if (!collect_ids(dir, ids))
        log_dieusys(LOG_EXIT_SYS, "read directory ", dir) ;
}

static void *rows_alloc(ssize_t n, size_t size)
{
    if (!n)
        return 0 ;

    void *p = malloc((size_t)n * size) ;
    if (!p)
        log_die(LOG_EXIT_SYS, "out of memory") ;

    return p ;
}

static char const *session_where(session_t const *s)
{
    if (s->tty[0])
        return s->tty ;

    if (s->display[0])
        return s->display ;

    return "-" ;
}

static void resolve_username(char const *statedir, uid_t uid, char *out)
{
    user_t u ;
    memset(&u, 0, sizeof(u)) ;

    if (state_load_user(statedir, uid, &u, 0, 0) && u.name[0]) {
        auto_strings(out, u.name) ;
        return ;
    }

    auto_strings(out, "unknown") ;
}

static void max_width(size_t *w, char const *s)
{
    size_t l = info_length_from_wchar(s) ;
    if (l > *w)
        *w = l ;
}


static void cmd_list_sessions(char const *statedir)
{
    _cleanup_sbl_ strbuf ids = SBL_ZERO ;
    collect_ids_or_die(statedir, "/sessions", &ids) ;

    srow_t *rows = rows_alloc((ssize_t)sbl_count(&ids), sizeof(srow_t)) ;

    size_t nr = 0 ;
    size_t w_id = strlen("ID"), w_uid = strlen("UID"), w_user = strlen("USER") ;
    size_t w_seat = strlen("SEAT"), w_where = strlen("TTY/DISPLAY") ;
    size_t w_state = strlen("STATE") ;

    size_t pos = 0 ;
    FOREACH_SBL(&ids, pos) {

        char const *id = ids.s + pos ;

        session_t s ;
        memset(&s, 0, sizeof(s)) ;
        if (!state_load_session(statedir, id, &s)) {
            log_warnusys("load session ", id) ;
            continue ;
        }

        srow_t *r = rows + nr ;
        auto_strings(r->id, id) ;
        r->uid[uid_format(r->uid, s.uid)] = 0 ;
        resolve_username(statedir, s.uid, r->user) ;
        auto_strings(r->seat, s.seat[0] ? s.seat : "-") ;
        auto_strings(r->where, session_where(&s)) ;
        r->state = session_state_str(s.state) ;

        max_width(&w_id, r->id) ;
        max_width(&w_uid, r->uid) ;
        max_width(&w_user, r->user) ;
        max_width(&w_seat, r->seat) ;
        max_width(&w_where, r->where) ;
        max_width(&w_state, r->state) ;
        nr++ ;
    }

    put_header("ID", w_id) ; put("  ") ;
    put_header("UID", w_uid) ; put("  ") ;
    put_header("USER", w_user) ; put("  ") ;
    put_header("SEAT", w_seat) ; put("  ") ;
    put_header("TTY/DISPLAY", w_where) ; put("  ") ;
    put_label("STATE") ;
    put_nl() ;

    for (size_t i = 0 ; i < nr ; i++) {
        srow_t *r = rows + i ;
        put_padded(r->id, w_id) ; put("  ") ;
        put_padded(r->uid, w_uid) ; put("  ") ;
        put_padded(r->user, w_user) ; put("  ") ;
        put_padded(r->seat, w_seat) ; put("  ") ;
        put_padded(r->where, w_where) ; put("  ") ;
        put(r->state) ;
        put_nl() ;
    }
    free(rows) ;

    if (!ostream_flush(ostream_1))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

static void cmd_list_users(char const *statedir)
{
    _cleanup_sbl_ strbuf ids = SBL_ZERO ;
    collect_ids_or_die(statedir, "/users", &ids) ;

    urow_t *rows = rows_alloc((ssize_t)sbl_count(&ids), sizeof(urow_t)) ;

    size_t nr = 0 ;
    size_t w_uid = strlen("UID"), w_name = strlen("NAME") ;
    size_t w_state = strlen("STATE"), w_ns = strlen("NSESSIONS") ;

    size_t pos = 0 ;
    FOREACH_SBL(&ids, pos) {

        char const *id = ids.s + pos ;

        uid_t uid ;
        if (!uid_parse(id, &uid))
            continue ; // collect_ids only yields decimal names, but never trust it

        user_t u ;
        memset(&u, 0, sizeof(u)) ;

        if (!state_load_user(statedir, uid, &u, 0, 0)) {
            log_warnusys("load user ", id) ;
            continue ;
        }

        urow_t *r = rows + nr ;
        auto_strings(r->uid, id) ;
        auto_strings(r->name, u.name[0] ? u.name : "unknown") ;
        r->state = user_state_str(u.state) ;
        r->nsessions[u32_fmt(r->nsessions, u.nsessions)] = 0 ;

        max_width(&w_uid, r->uid) ;
        max_width(&w_name, r->name) ;
        max_width(&w_state, r->state) ;
        max_width(&w_ns, r->nsessions) ;
        nr++ ;
    }

    put_header("UID", w_uid) ; put("  ") ;
    put_header("NAME", w_name) ; put("  ") ;
    put_header("STATE", w_state) ; put("  ") ;
    put_label("NSESSIONS") ; put_nl() ;

    for (size_t i = 0 ; i < nr ; i++) {
        urow_t *r = rows + i ;
        put_padded(r->uid, w_uid) ; put("  ") ;
        put_padded(r->name, w_name) ; put("  ") ;
        put_padded(r->state, w_state) ; put("  ") ;
        put(r->nsessions) ; put_nl() ;
    }

    free(rows) ;

    if (!ostream_flush(ostream_1))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

static void cmd_session_status(char const *statedir, char const *id)
{
    session_t s ;
    memset(&s, 0, sizeof(s)) ;

    if (!state_load_session(statedir, id, &s)) {
        if (errno == ENOENT)
            log_die(LOG_EXIT_USER, "no such session: ", id) ;
        log_dieusys(LOG_EXIT_SYS, "load session ", id) ;
    }

    char user[USER_NAME_MAX] ;
    resolve_username(statedir, s.uid, user) ;

    enum { F_ID, F_UID, F_USER, F_LEADER, F_SEAT, F_VTNR, F_TYPE, F_CLASS,
           F_TTY, F_DISPLAY, F_REMOTE, F_SERVICE, F_RUSER, F_RHOST, F_STATE,
           F_TIMESTAMP, F_N } ;
    char buf[F_N][INFO_FIELD_MAXLEN] = {
        "Id", "Uid", "User", "Leader", "Seat", "VTNr", "Type", "Class",
        "TTY", "Display", "Remote", "Service", "RemoteUser", "RemoteHost",
        "State", "Timestamp" } ;
    char fields[F_N][INFO_FIELD_MAXLEN] = {{ 0 }} ;
    info_field_align(buf, fields, field_suffix, F_N) ;

    put_field(fields[F_ID], id) ;
    put_field_u64(fields[F_UID], s.uid) ;
    put_field(fields[F_USER], user) ;
    put_field_u64(fields[F_LEADER], (uint64_t)s.leader) ;
    put_field(fields[F_SEAT], s.seat) ;
    put_field_u64(fields[F_VTNR], s.vtnr) ;
    put_field(fields[F_TYPE], session_type_str(s.type)) ;
    put_field(fields[F_CLASS], session_class_str(s.class)) ;
    put_field(fields[F_TTY], s.tty) ;
    put_field(fields[F_DISPLAY], s.display) ;
    put_field(fields[F_REMOTE], s.remote ? "yes" : "no") ;
    put_field(fields[F_SERVICE], s.service) ;
    put_field(fields[F_RUSER], s.remote_user) ;
    put_field(fields[F_RHOST], s.remote_host) ;
    put_field(fields[F_STATE], session_state_str(s.state)) ;
    put_field_u64(fields[F_TIMESTAMP], s.timestamp) ;

    if (!ostream_flush(ostream_1))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

static void print_user_sessions(char const *statedir, char const *list)
{
    put("Sessions:") ; put_nl() ;

    if (!list[0])
        return ;

    _cleanup_strbuf_ strbuf sb = STRBUF_ZERO ;
    if (!sbl_clean_string(&sb, list))
        log_dieusys(LOG_EXIT_SYS, "parse session list") ;

    size_t pos = 0 ;
    FOREACH_SBL(&sb, pos) {

        char const *id = sb.s + pos ;
        session_t s ;
        memset(&s, 0, sizeof(s)) ;

        put("  ") ;
        put(id) ;
        put("  ") ;

        if (state_load_session(statedir, id, &s)) {
            put(session_where(&s)) ;
            put("  ") ;
            put(session_state_str(s.state)) ;
        } else {
            put("(state file missing)") ;
        }

        put_nl() ;
    }
}

static void cmd_user_status(char const *statedir, char const *arg)
{
    uid_t uid ;
    size_t l = uid_parse(arg, &uid) ;
    if (!l || arg[l]) {
        struct passwd *pw = getpwnam(arg) ;
        if (!pw)
            log_die(LOG_EXIT_USER, "no such user: ", arg) ;
        uid = pw->pw_uid ;
    }

    user_t u ;
    memset(&u, 0, sizeof(u)) ;
    char sessions[PROTO_PAYLOAD_MAX] ;

    if (!state_load_user(statedir, uid, &u, sessions, sizeof(sessions))) {
        if (errno == ENOENT)
            log_die(LOG_EXIT_USER, "no such user: ", arg) ;
        log_dieusys(LOG_EXIT_SYS, "load user ", arg) ;
    }

    enum { U_UID, U_NAME, U_GID, U_STATE, U_NSESSIONS, U_TIMESTAMP, U_N } ;
    char buf[U_N][INFO_FIELD_MAXLEN] = {
        "Uid", "Name", "Gid", "State", "NSessions", "Timestamp" } ;
    char fields[U_N][INFO_FIELD_MAXLEN] = {{ 0 }} ;
    info_field_align(buf, fields, field_suffix, U_N) ;

    put_field_u64(fields[U_UID], u.uid) ;
    put_field(fields[U_NAME], u.name) ;
    put_field_u64(fields[U_GID], u.gid) ;
    put_field(fields[U_STATE], user_state_str(u.state)) ;
    put_field_u64(fields[U_NSESSIONS], u.nsessions) ;
    put_field_u64(fields[U_TIMESTAMP], u.timestamp) ;
    print_user_sessions(statedir, sessions) ;

    if (!ostream_flush(ostream_1))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

static int do_list_users(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;

    cmd_list_users(((ctx_t const *)data)->statedir) ;

    return 0 ;
}

static int do_list_sessions(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;

    cmd_list_sessions(((ctx_t const *)data)->statedir) ;

    return 0 ;
}

static int do_status_users(int argc, char const *const *argv, void *data)
{
    if (argc < 1)
        log_die(LOG_EXIT_USER, "status users requires a uid or a login name") ;

    cmd_user_status(((ctx_t const *)data)->statedir, argv[0]) ;

    return 0 ;
}

static int do_status_sessions(int argc, char const *const *argv, void *data)
{
    if (argc < 1)
        log_die(LOG_EXIT_USER, "status sessions requires a session id") ;

    cmd_session_status(((ctx_t const *)data)->statedir, argv[0]) ;

    return 0 ;
}

static int access_edit(int argc, char const *const *argv, int add)
{
    if (argc < 1)
        log_die(LOG_EXIT_USER, add ? "access allow requires a username" : "access deny requires a username") ;

    if (geteuid())
        log_die(LOG_EXIT_USER, "only root may edit shutdown.allow") ;

    char const *user = argv[0] ;
    if (!(add ? shutdown_add(user) : shutdown_remove(user))) {

        if (errno == EINVAL)
            log_die(LOG_EXIT_USER, "invalid username: ", user) ;

        log_dieusys(LOG_EXIT_SYS, add ? "add to shutdown.allow" : "remove from shutdown.allow") ;
    }

    return 0 ;
}

static int do_access_allow(int argc, char const *const *argv, void *data)
{
    (void)data ;
    return access_edit(argc, argv, 1) ;
}

static int do_access_deny(int argc, char const *const *argv, void *data)
{
    (void)data ;
    return access_edit(argc, argv, 0) ;
}

static int do_access_list(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ; (void)data ;

    _cleanup_strbuf_ strbuf out = STRBUF_ZERO ;
    if (!shutdown_list(&out))
        log_dieusys(LOG_EXIT_SYS, "read shutdown.allow") ;

    if (out.len && !ostream_putflush(ostream_1, out.s, out.len))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;

    return 0 ;
}

static int power_request(power_action_t action, int force)
{
    char payload[PAYLOAD_MAX] ;
    size_t len = strlen(power_action_str(action)) + 16 ;

    if (len >= PAYLOAD_MAX) {
        errno = ENAMETOOLONG ;
        log_diesys(LOG_EXIT_USER, "action name too long") ;
    }

    auto_strings(payload, "ACTION=", power_action_str(action), force ? "\nFORCE=1" : "") ;

    uint16_t rop = 0, rlen = 0 ;
    char reply[PROTO_PAYLOAD_MAX + 1] ;

    if (!userd_call(USERD_SOCKET_PATH, PROTO_POWER, payload, (uint16_t)strlen(payload), &rop, reply, PROTO_PAYLOAD_MAX, &rlen))
        log_dieusys(LOG_EXIT_SYS, "reach 66-userd") ;

    reply[rlen] = 0 ;
    if (rop != PROTO_OK)
        log_die(LOG_EXIT_USER, "power request denied: ", rlen ? reply : "(no detail)") ;

    return 0 ;
}

static int on_power(int id, char const *arg, void *data)
{
    (void)arg ;

    if (id == 'f')
        ((ctx_t *)data)->force = 1 ;

    return 0 ;
}

static int do_poweroff(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;
    return power_request(POWER_POWEROFF, ((ctx_t const *)data)->force) ;
}

static int do_reboot(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;
    return power_request(POWER_REBOOT, ((ctx_t const *)data)->force) ;
}

static int do_halt(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;
    return power_request(POWER_HALT, ((ctx_t const *)data)->force) ;
}

static int do_suspend(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;
    return power_request(POWER_SUSPEND, ((ctx_t const *)data)->force) ;
}

static int do_hibernate(int argc, char const *const *argv, void *data)
{
    (void)argc ; (void)argv ;
    return power_request(POWER_HIBERNATE, ((ctx_t const *)data)->force) ;
}

static int on_global(int id, char const *arg, void *data)
{
    ctx_t *ctx = data ;

    switch (id) {
        case 'z' : ctx->icolor = 1 ; break ;
        case 'v' : if (!u32_scan_strict(arg, &VERBOSITY)) log_die(LOG_EXIT_USER, "invalid verbosity: ", arg) ; break ;
    }
    return 0 ;
}

static opt_t const power_opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",  .arg = OPT_NONE, .help = "print this help" },
    { .id = 'f',         .shortname = 'f', .longname = "force", .arg = OPT_NONE, .help = "proceed even if other users are logged in" },
} ;

static opt_t const root_opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",      .arg = OPT_NONE,                          .help = "print this help" },
    { .id = 'z',         .shortname = 'z', .longname = "color",     .arg = OPT_NONE,                          .help = "enable color (only when stdout is a TTY)" },
    { .id = 'v',         .shortname = 'v', .longname = "verbosity", .arg = OPT_REQUIRED, .argname = "number", .help = "set verbosity level" },
} ;

// help-only option table for commands that take no option of their own but must still answer -h.
static opt_t const help_opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .arg = OPT_NONE, .help = "print this help" },
} ;

static opt_cmd_t const list_sub[] = {
    { .name = "users",    .help = "list active users",    .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_list_users },
    { .name = "sessions", .help = "list active sessions", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_list_sessions },
} ;

static opt_cmd_t const status_sub[] = {
    { .name = "users",    .help = "show one user and its sessions", .operands = "<id|name>", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_status_users },
    { .name = "sessions", .help = "show one session",               .operands = "<id>", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_status_sessions },
} ;

static opt_cmd_t const access_sub[] = {
    { .name = "allow", .help = "add a user to the power-off whitelist (root only)",      .operands = "<user>", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_access_allow },
    { .name = "deny",  .help = "remove a user from the power-off whitelist (root only)", .operands = "<user>", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_access_deny },
    { .name = "list",  .help = "list users allowed to power off",                        .opts = help_opts, .nopts = OPT_COUNT(help_opts), .fn = &do_access_list },
} ;

static opt_cmd_t const root_sub[] = {
    { .name = "list",     .help = "list active users or sessions", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .sub = list_sub,   .nsub = OPT_COUNT(list_sub) },
    { .name = "status",   .help = "show one user or session",      .opts = help_opts, .nopts = OPT_COUNT(help_opts), .sub = status_sub, .nsub = OPT_COUNT(status_sub) },
    { .name = "access",   .help = "manage the shutdown.allow power-off whitelist", .opts = help_opts, .nopts = OPT_COUNT(help_opts), .sub = access_sub, .nsub = OPT_COUNT(access_sub) },
    { .name = "poweroff",  .help = "power off the machine", .opts = power_opts, .nopts = OPT_COUNT(power_opts), .on_option = &on_power, .fn = &do_poweroff },
    { .name = "reboot",    .help = "reboot the machine",    .opts = power_opts, .nopts = OPT_COUNT(power_opts), .on_option = &on_power, .fn = &do_reboot },
    { .name = "halt",      .help = "halt the machine",      .opts = power_opts, .nopts = OPT_COUNT(power_opts), .on_option = &on_power, .fn = &do_halt },
    { .name = "suspend",   .help = "suspend the machine to RAM",   .opts = power_opts, .nopts = OPT_COUNT(power_opts), .on_option = &on_power, .fn = &do_suspend },
    { .name = "hibernate", .help = "hibernate the machine to disk", .opts = power_opts, .nopts = OPT_COUNT(power_opts), .on_option = &on_power, .fn = &do_hibernate },
} ;

static opt_cmd_t const root_cmd = {
    .name = "66-userctl",
    .opts = root_opts,
    .nopts = OPT_COUNT(root_opts),
    .on_option = &on_global,
    .sub = root_sub,
    .nsub = OPT_COUNT(root_sub),
} ;

int main(int argc, char const *const *argv)
{
    PROG = "66-userctl" ;
    log_color = &log_color_disable ;
    setlocale(LC_ALL, "") ;

    ctx_t ctx = { .statedir = USERD_STATEDIR, .icolor = 0, .force = 0 } ;

    {
        opt_scan_t st = OPT_SCAN_ZERO ;
        for (;;) {

            int o = opt_scan(argc, argv, root_opts, OPT_COUNT(root_opts), &st) ;
            if (o == OPT_END || o == OPT_ID_HELP || o == OPT_UNKNOWN || o == OPT_MISSARG)
                break ;

            on_global(o, st.arg, &ctx) ;
        }
    }

    if (ctx.icolor)
        log_color = isatty(1) ? &log_color_enable : &log_color_disable ;

    return opt_dispatch(argc, argv, &root_cmd, &ctx) ;
}
