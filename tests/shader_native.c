#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <shaderc/shaderc.h>
#include "nvn/maxwell/shader.h"

static void compile(ShaderInfo *info){
    assert(info->msl);shaderc_compiler_t compiler=shaderc_compiler_initialize();assert(compiler);
    shaderc_shader_kind kind=info->stage==STAGE_FRAGMENT?shaderc_fragment_shader:shaderc_vertex_shader;
    shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,info->msl,strlen(info->msl),kind,"synthetic","main",NULL);
    if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success){fputs(shaderc_result_get_error_message(result),stderr);assert(0);}
    assert(shaderc_result_get_length(result)>20);shaderc_result_release(result);shaderc_compiler_release(compiler);
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    uint8_t blob[512]={0};uint64_t *code=(uint64_t *)(blob+0x80);ShaderKey key={.glsl=1};
    code[1]=0x50e2432111170000ull; // VOTE.VTG.RA clip reduction hint
    code[2]=0xe30000000007001cull; // EXIT.FCSM_TR must not terminate every vertex
    code[3]=0xefd8000000000000ull|(255ull<<8)|(7ull<<16)|(0x80ull<<20)|(3ull<<47);
    code[5]=0xeff0000000000000ull|(255ull<<8)|(7ull<<16)|(0x70ull<<20)|(3ull<<47);
    code[6]=0xe30000000007000full; // ordinary unconditional exit after exports
    ShaderInfo *info=maxwell_translate(blob,sizeof blob,STAGE_VERTEX,&key);assert(info->msl);
    const char *export=strstr(info->msl,"gl_Position[0] ="),*exit=strstr(info->msl,"break;");
    assert(export&&exit&&export<exit);compile(info);maxwell_free(info);
    puts("PASS synthetic clip reduction preserves position export and compiles to SPIR-V");
    code[1]=0x36d7008000170413ull;code[2]=0x50b0000000070f00ull;
    info=maxwell_translate(blob,sizeof blob,STAGE_VERTEX,&key);assert(info->msl);compile(info);maxwell_free(info);
    code[1]|=1ull<<46; // extended carry-in form must remain explicit until implemented
    info=maxwell_translate(blob,sizeof blob,STAGE_VERTEX,&key);assert(!info->msl&&strstr(info->error,"extended LEA"));maxwell_free(info);
    puts("PASS ordinary LEA compiles; unsupported carry form is rejected");
    memset(blob,0,sizeof blob);*(uint32_t *)(blob+0x78)=0xf;
    // Original TMML fixture: t2d, handle index 8, component Y -> destination r7.
    code[1]=0xdf58000000000000ull|(8ull<<36)|(2ull<<31)|(1ull<<29)|(7ull<<16)|7;
    code[2]=0x50b0000000070f00ull;
    code[3]=0xe30000000007000full;
    info=maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key);assert(info->msl);
    assert(info->ntex==1&&info->tex[0].handle_offset==32);
    assert(strstr(info->msl,"textureQueryLod(t0")&&strstr(info->msl,"r7 = uint(int(tmml_lod[0] * 256.0f))"));
    compile(info);maxwell_free(info);
    code[1]=(code[1]&~(15ull<<31))|(3ull<<31);
    info=maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key);assert(info->msl);
    assert(strstr(info->msl,"r7 = uint(int(tmml_lod[1] * 256.0f))")&&strstr(info->msl,"r8 = uint(int(tmml_lod[0] * 256.0f))"));
    compile(info);maxwell_free(info);
    code[1]|=1ull<<28;
    info=maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key);assert(!info->msl&&strstr(info->error,"TMML"));maxwell_free(info);
    puts("PASS fragment TMML LOD mask, reversed fixed-point components, binding and SPIR-V");return 0;
}
