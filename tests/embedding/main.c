/* SPDX-License-Identifier: Apache-2.0 */
#define STATIC_ERLANG_NIF
#define STATIC_ERLANG_NIF_LIBNAME beam_re_control
#include <erl_nif.h>
#include <beam/beam.h>
#include <beam/host.h>
#include <beam/native_image.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "fixture.h"
#include "../builtin_bindings.h"
#ifdef LINKED_BINDING
#include BINDING_HEADER
extern size_t beam_re_test_allocations(void);
extern size_t beam_re_test_resources(void);
extern int beam_re_test_set_limit(int);
extern size_t beam_re_test_yield_coverage(const int **, const uint64_t **, const uint64_t **);
extern size_t beam_re_test_compile_entered(void), beam_re_test_compile_active(void);
extern unsigned beam_re_test_compile_hold(unsigned);
extern unsigned beam_re_test_fail_load(unsigned);
#endif

static size_t (*allocation_count)(void), (*resource_count)(void);
static int (*set_limit)(int);
static size_t (*yield_coverage)(const int **, const uint64_t **, const uint64_t **);
static size_t (*compile_entered)(void), (*compile_active)(void);
static unsigned (*compile_hold)(unsigned);
static unsigned (*fail_load)(unsigned);

/* Stock PCRE has no restart-budget API. An unnamespaced adapted backend must
 * never bind to these deliberately incompatible host definitions. */
void pcre2_set_loops_left_8(void *unused, int limit) { (void)unused; (void)limit; abort(); }
int pcre2_get_loops_left_8(void *unused) { (void)unused; abort(); }

static ERL_NIF_TERM stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc; (void)argv;
    return enif_make_tuple2(env, enif_make_uint64(env, allocation_count()),
                          enif_make_uint64(env, resource_count()));
}
static ERL_NIF_TERM limit(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int value; (void)argc;
    if (enif_is_identical(argv[0], enif_make_atom(env,"default"))) value = -1;
    else if (!enif_get_int(env,argv[0],&value) || value < -1 || value == 0) return enif_make_badarg(env);
    return enif_make_int(env, set_limit(value));
}
static ERL_NIF_TERM compile_stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc; (void)argv;
    return enif_make_tuple2(env,enif_make_uint64(env,compile_entered()),enif_make_uint64(env,compile_active()));
}
static ERL_NIF_TERM hold_compile(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    unsigned milliseconds; (void)argc;
    if (!enif_get_uint(env,argv[0],&milliseconds) || milliseconds>1000) return enif_make_badarg(env);
    return enif_make_uint(env,compile_hold(milliseconds));
}
static ERL_NIF_TERM coverage(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc; (void)argv;
    const int *lines; const uint64_t *visits, *yields;
    size_t count = yield_coverage(&lines,&visits,&yields);
    ERL_NIF_TERM result = enif_make_list(env,0);
    while (count) {
        --count;
        result = enif_make_list_cell(env,enif_make_tuple3(env,enif_make_int(env,lines[count]),
            enif_make_uint64(env,visits[count]),enif_make_uint64(env,yields[count])),result);
    }
    return result;
}
static ERL_NIF_TERM read_file(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary path; (void)argc;
    if (!enif_inspect_iolist_as_binary(env,argv[0],&path) || memchr(path.data,0,path.size)) return enif_make_badarg(env);
    char *name = enif_alloc(path.size+1);
    if (!name) return enif_raise_exception(env,enif_make_atom(env,"enomem"));
    memcpy(name,path.data,path.size); name[path.size]=0;
    FILE *file = fopen(name,"rb"); enif_free(name);
    if (!file) return enif_make_tuple2(env,enif_make_atom(env,"error"),enif_make_atom(env,"enoent"));
    if (fseek(file,0,SEEK_END)) { fclose(file); return enif_make_badarg(env); }
    long size = ftell(file);
    if (size < 0 || fseek(file,0,SEEK_SET)) { fclose(file); return enif_make_badarg(env); }
    ErlNifBinary data;
    if (!enif_alloc_binary((size_t)size,&data)) { fclose(file); return enif_raise_exception(env,enif_make_atom(env,"enomem")); }
    size_t got=fread(data.data,1,data.size,file); int failed=ferror(file); fclose(file);
    if (failed || got != data.size) { enif_release_binary(&data); return enif_make_tuple2(env,enif_make_atom(env,"error"),enif_make_atom(env,"eio")); }
    return enif_make_tuple2(env,enif_make_atom(env,"ok"),enif_make_binary(env,&data));
}
static ERL_NIF_TERM write_data(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary data; (void)argc;
    if (!enif_inspect_iolist_as_binary(env,argv[0],&data)) return enif_make_badarg(env);
    if (fwrite(data.data,1,data.size,stdout) != data.size) return enif_make_badarg(env);
    fflush(stdout); return enif_make_atom(env,"ok");
}
extern ERL_NIF_TERM beam_re_test_corrupt_export(ErlNifEnv *,int,const ERL_NIF_TERM []);
static ErlNifFunc controls[] = {
    {"stats",0,stats,0}, {"set_limit",1,limit,0}, {"coverage",0,coverage,0},
    {"compile_stats",0,compile_stats,0}, {"compile_hold",1,hold_compile,0},
    {"corrupt_export",2,beam_re_test_corrupt_export,0},
    {"read_file",1,read_file,ERL_NIF_DIRTY_JOB_IO_BOUND}, {"write",1,write_data,ERL_NIF_DIRTY_JOB_IO_BOUND}};
ERL_NIF_INIT(beam_re_test,controls,NULL,NULL,NULL,NULL)
static ErlNifEntry *control_init(void) { return beam_re_control_nif_init(); }
extern ErlNifEntry *beam_re_tracer_nif_init(void);

static int entries(const char *path)
{
    DIR *dir=opendir(path); if (!dir) return -1;
    int count=0; struct dirent *entry;
    while ((entry=readdir(dir))) if (entry->d_name[0]!='.') ++count;
    closedir(dir); return count;
}
static int stock_pcre(void)
{
    int code; PCRE2_SIZE offset;
    pcre2_code *pattern=pcre2_compile((PCRE2_SPTR)"^(hello) ([0-9]+)$",PCRE2_ZERO_TERMINATED,0,&code,&offset,NULL);
    if (!pattern) return 0;
    pcre2_match_data *data=pcre2_match_data_create_from_pattern(pattern,NULL);
    int matched=data && pcre2_match(pattern,(PCRE2_SPTR)"hello 42",8,0,0,data,NULL)==3;
    pcre2_match_data_free(data); pcre2_code_free(pattern); return matched;
}
static int call(beam_vm *vm,const char *module,const char *function,const unsigned char *args,size_t size,int exception)
{
    beam_result result={0}; beam_status status=beam_vm_call(vm,module,function,(beam_bytes){args,size},&result);
    const unsigned char *value=result.value.data;
    int exact_ok=(result.value.size==5 && value[0]==131 && (value[1]==115 || value[1]==119) &&
                  value[2]==2 && value[3]=='o' && value[4]=='k') ||
                 (result.value.size==6 && value[0]==131 && (value[1]==100 || value[1]==118) &&
                  value[2]==0 && value[3]==2 && value[4]=='o' && value[5]=='k');
    int passed=status==BEAM_OK && (exception ? result.kind==BEAM_EXCEPTION :
        result.kind==BEAM_RETURN && exact_ok);
    if (!passed) {
        fprintf(stderr,"%s:%s: status=%d kind=%d value=",module,function,status,result.kind);
        for (size_t i=0;i<result.value.size && i<3000;++i) fprintf(stderr,"%02x",result.value.data[i]);
        fputc('\n',stderr);
    }
    beam_result_release(&result); return passed;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"embedding:%d: %s (%s)\n",__LINE__,#x,error.message); return 1; } } while(0)
int main(int argc,char **argv)
{
    beam_error error={0}; beam_system *system=NULL; beam_vm *vm=NULL;
    beam_native_image *image=NULL; beam_native_owner owner={.struct_size=sizeof(owner)};
    beam_nif_binding bindings[3]={{.struct_size=sizeof(beam_nif_binding)}};
    int argument=1;
#ifdef LINKED_BINDING
    bindings[0]=*LINKED_BINDING();
    allocation_count=beam_re_test_allocations; resource_count=beam_re_test_resources;
    set_limit=beam_re_test_set_limit; yield_coverage=beam_re_test_yield_coverage;
    compile_entered=beam_re_test_compile_entered; compile_active=beam_re_test_compile_active;
    compile_hold=beam_re_test_compile_hold;
    fail_load=beam_re_test_fail_load;
#else
    CHECK(argc>argument);
    CHECK(beam_host_open_nif(argv[argument++],"re",&image,&bindings[0],&owner,&error)==BEAM_OK);
    void *symbol;
#define SYMBOL(target,name) do { CHECK(beam_native_image_symbol(image,name,&symbol)==BEAM_OK); memcpy(&target,&symbol,sizeof(target)); } while(0)
    SYMBOL(allocation_count,"beam_re_test_allocations"); SYMBOL(resource_count,"beam_re_test_resources");
    SYMBOL(set_limit,"beam_re_test_set_limit"); SYMBOL(yield_coverage,"beam_re_test_yield_coverage");
    SYMBOL(compile_entered,"beam_re_test_compile_entered"); SYMBOL(compile_active,"beam_re_test_compile_active");
    SYMBOL(compile_hold,"beam_re_test_compile_hold");
    SYMBOL(fail_load,"beam_re_test_fail_load");
#endif
    bindings[1]=(beam_nif_binding){sizeof(beam_nif_binding),"beam_re_test",beam_build_id(),control_init,NULL};
    bindings[2]=(beam_nif_binding){sizeof(beam_nif_binding),"erl_tracer",beam_build_id(),beam_re_tracer_nif_init,NULL};
    int selected_case=argument<argc && !strcmp(argv[argument],"--case");
    int conformance=selected_case || (argument<argc && !strcmp(argv[argument],"--suite"));
    const beam_builtin_binding *native=conformance ? re_test_canonical_bindings : re_test_modern_bindings;
    size_t native_count=conformance ? sizeof re_test_canonical_bindings/sizeof *native : sizeof re_test_modern_bindings/sizeof *native;
    beam_composition_options spec={.struct_size=sizeof(spec),
        .module_count=conformance ? fixture_canonical_count : fixture_modern_count,
        .modules=conformance ? fixture_canonical_modules : fixture_modern_modules,
        .binding_count=3,.bindings=bindings,
        .builtin_binding_count=native_count,.builtin_bindings=native};
#ifndef LINKED_BINDING
    spec.native_owner_count=1; spec.native_owners=&owner;
#endif
    beam_composition *composition=NULL;
    CHECK(stock_pcre());
    CHECK(beam_composition_prepare(&spec,&composition,&error)==BEAM_OK);
    CHECK(beam_init(&system,&error)==BEAM_OK);
    int threads=entries("/proc/self/task"),fds=entries("/proc/self/fd");
    const unsigned char nil[]={131,106};
    int iterations=conformance ? 1 : 6;
    for (unsigned stage=1;stage<=2;++stage) {
        beam_vm_options options; beam_vm_options_init(&options); options.composition=composition;
        options.schedulers=options.schedulers_online=1;
        options.dirty_cpu_schedulers=options.dirty_cpu_schedulers_online=1;
        options.dirty_io_schedulers=1;
        CHECK(beam_vm_init(system,&options,&vm,&error)==BEAM_OK);
        CHECK(beam_vm_start(vm,&error)==BEAM_OK);
        (void)fail_load(stage);
        CHECK(beam_vm_bind_nif(vm,"re",&error)!=BEAM_OK);
        CHECK(beam_vm_terminate(&vm,NULL)==BEAM_OK);
        CHECK(allocation_count()==0 && resource_count()==0);
    }
    for (int i=0;i<iterations;++i) {
        beam_vm_options options; beam_vm_options_init(&options); options.composition=composition;
        options.schedulers=options.schedulers_online=1;
        options.dirty_cpu_schedulers=options.dirty_cpu_schedulers_online=1;
        options.dirty_io_schedulers=1;
        CHECK(beam_vm_init(system,&options,&vm,&error)==BEAM_OK);
        CHECK(beam_vm_start(vm,&error)==BEAM_OK);
        CHECK(call(vm,"re_checks","run",nil,sizeof(nil),1));
        CHECK(beam_vm_bind_nif(vm,"re",&error)==BEAM_OK);
        CHECK(beam_vm_bind_nif(vm,"re",&error)==BEAM_OK);
        CHECK(beam_vm_bind_nif(vm,"beam_re_test",&error)==BEAM_OK);
        CHECK(beam_vm_bind_nif(vm,"erl_tracer",&error)==BEAM_OK);
        CHECK(call(vm,"re_checks","run",nil,sizeof(nil),0));
        CHECK(call(vm,"import_bounds_checks","run",nil,sizeof(nil),0));
        CHECK(call(vm,"re_checks","share_checks",nil,sizeof(nil),0));
        CHECK(call(vm,"re_checks","yield_checks",nil,sizeof(nil),0));
        CHECK(call(vm,"re_checks","resource_checks",nil,sizeof(nil),0));
        if (conformance) {
            CHECK(argument+1<argc);
            size_t n=strlen(argv[argument+1]); CHECK(n<65536);
            size_t m=selected_case && argument+2<argc ? strlen(argv[argument+2]) : 0;
            CHECK(!selected_case || (m>0 && m<256));
            size_t size=n+(selected_case ? m+14 : 12);
            unsigned char *args=malloc(size); CHECK(args);
            const unsigned char head[]={131,108,0,0,0,1,109}; memcpy(args,head,sizeof(head));
            if (selected_case) args[5]=2;
            args[7]=(unsigned char)(n>>24); args[8]=(unsigned char)(n>>16);
            args[9]=(unsigned char)(n>>8); args[10]=(unsigned char)n;
            memcpy(args+11,argv[argument+1],n);
            if (selected_case) {
                args[n+11]=119; args[n+12]=(unsigned char)m;
                memcpy(args+n+13,argv[argument+2],m);
            }
            args[size-1]=106;
            int passed=call(vm,"re_conformance",selected_case ? "run_case" : "run",args,size,0);
            free(args); CHECK(passed);
        }
        CHECK(call(vm,"re_checks","retain",nil,sizeof(nil),0));
        CHECK(allocation_count()>0 && resource_count()>0);
        CHECK(beam_vm_terminate(&vm,NULL)==BEAM_OK);
        CHECK(allocation_count()==0 && resource_count()==0);
        for (int j=0;j<100 && entries("/proc/self/task")!=threads;++j) usleep(1000);
        CHECK(entries("/proc/self/task")==threads && entries("/proc/self/fd")==fds);
        CHECK(stock_pcre());
    }
    beam_composition_release(&composition);
    CHECK(beam_terminate(&system)==BEAM_OK);
    beam_native_image_release(&image);
    puts("RE_EMBEDDING_OK"); return 0;
}
