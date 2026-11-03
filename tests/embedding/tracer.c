/* SPDX-License-Identifier: Apache-2.0 */
/* Test-owned implementation of the public erl_tracer NIF behavior. Only the
 * ordinary running-process trace messages used by acceptance are selected. */
#define STATIC_ERLANG_NIF
#define STATIC_ERLANG_NIF_LIBNAME beam_re_tracer
#include <erl_nif.h>
static ERL_NIF_TERM enabled(ErlNifEnv *env,int argc,const ERL_NIF_TERM argv[])
{
    (void)argc;
    ErlNifPid receiver;
    if (!enif_get_local_pid(env,argv[1],&receiver) || !enif_is_process_alive(env,&receiver))
        return enif_make_atom(env,enif_is_identical(argv[0],enif_make_atom(env,"trace_status")) ? "remove" : "discard");
    if (enif_is_identical(argv[1],argv[2])) return enif_make_atom(env,"discard");
    return enif_make_atom(env,"trace");
}
static ERL_NIF_TERM trace_message(ErlNifEnv *env,int argc,const ERL_NIF_TERM argv[])
{
    (void)argc;
    ErlNifPid receiver;
    if (enif_get_local_pid(env,argv[1],&receiver)) {
        ERL_NIF_TERM message=enif_make_tuple4(env,enif_make_atom(env,"trace"),argv[2],argv[0],argv[3]);
        (void)enif_send(env,&receiver,NULL,message);
    }
    return enif_make_atom(env,"ok");
}
static ErlNifFunc functions[]={{"enabled",3,enabled,0},{"trace",5,trace_message,0}};
ERL_NIF_INIT(erl_tracer,functions,NULL,NULL,NULL,NULL)
