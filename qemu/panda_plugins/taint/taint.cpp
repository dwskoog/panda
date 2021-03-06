/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */

/*
 * PANDA taint analysis plugin
 * Ryan Whelan, Tim Leek, Sam Coe, Nathan VanBenschoten
 */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

extern "C" {

#include "qemu-common.h"
#include "cpu-all.h"
#ifndef CONFIG_SOFTMMU
#include "syscall_defs.h"
#endif

#include <sys/time.h>
#include "panda_plugin.h"
#include "panda_memlog.h"
#include "panda_stats.h"
#include "panda/network.h"
#ifdef CONFIG_SOFTMMU
#include "rr_log.h"
#endif

}

#include "llvm/PassManager.h"
#include "llvm/PassRegistry.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"

#include "tcg-llvm.h"

#include "llvm_taint_lib.h"
#include "panda_dynval_inst.h"
#include "taint_processor.h"


// Enable callbacks that deal with network taint
bool use_network_taint = 0;

// Label all incoming network traffic as tainted
bool taint_label_incoming_network_traffic = 0;

// Query all outgoing network traffic for taint
bool taint_query_outgoing_network_traffic = 0;

// defined in panda/taint_processor.c
extern uint32_t max_taintset_card;
extern uint32_t max_taintset_compute_number;


// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {


bool init_plugin(void *);
void uninit_plugin(void *);
int before_block_exec(CPUState *env, TranslationBlock *tb);
int after_block_exec(CPUState *env, TranslationBlock *tb,
    TranslationBlock *next_tb);
int cb_cpu_restore_state(CPUState *env, TranslationBlock *tb);
int guest_hypercall_callback(CPUState *env);

// for hd taint
int cb_replay_hd_transfer_taint
  (CPUState *env,
   uint32_t type,
   uint64_t src_addr,
   uint64_t dest_addr,
   uint32_t num_bytes);

int handle_packet(CPUState *env, uint8_t *buf, int size, uint8_t direction,
    uint64_t old_buf_addr);

// for network taint
int cb_replay_net_transfer_taint(CPUState *env, uint32_t type,
   uint64_t src_addr, uint64_t dest_addr, uint32_t num_bytes);

int cb_replay_cpu_physical_mem_rw_ram
  (CPUState *env,
   uint32_t is_write, uint8_t *src_addr, uint64_t dest_addr, uint32_t num_bytes);


#ifndef CONFIG_SOFTMMU
int user_after_syscall(void *cpu_env, bitmask_transtbl *fcntl_flags_tbl,
                       int num, abi_long arg1, abi_long arg2, abi_long arg3,
                       abi_long arg4, abi_long arg5, abi_long arg6, abi_long
                       arg7, abi_long arg8, void *p, abi_long ret);

#endif
int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf);
int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf);



Shad *shadow = NULL; // Global shadow memory

}

// Pointer passed in init_plugin()
void *plugin_ptr = NULL;

// Our pass manager to derive taint ops
llvm::FunctionPassManager *taintfpm = NULL;

// Taint and instrumentation function passes
llvm::PandaTaintFunctionPass *PTFP = NULL;
llvm::PandaInstrFunctionPass *PIFP = NULL;

// Global count of taint labels
int count = 0;

// For now, taint becomes enabled when a label operation first occurs, and
// becomes disabled when a query operation subsequently occurs
bool taintEnabled = false;

// Lets us know right when taint was enabled
bool taintJustEnabled = false;

// Lets us know right when taint was disabled
bool taintJustDisabled = false;

// Globals needed for taint io buffer
TaintOpBuffer *tob_io_thread;
uint32_t       tob_io_thread_max_size = 1024 * 1024;

// Apply taint to a buffer of memory
void add_taint(CPUState *env, Shad *shad, TaintOpBuffer *tbuf,
        uint64_t addr, int length){
    struct addr_struct a = {};
    a.typ = MADDR;
    struct taint_op_struct op = {};
    op.typ = LABELOP;
    for (int i = 0; i < length; i++){
#ifdef CONFIG_SOFTMMU

      target_phys_addr_t pa = cpu_get_phys_addr(env, addr + i);

      if (pa == -1) {
	printf("can't label addr=0x%lx: mmu hasn't mapped virt->phys, i.e., it isnt actually there.\n", addr +i);
	continue;
      }
      assert (pa != -1);
      a.val.ma = pa;

#else
        a.val.ma = addr + i;
#endif // CONFIG_SOFTMMU
        op.val.label.a = a;
        op.val.label.l = i + count; // byte label
        //op.val.label.l = 1; // binary label
        tob_op_write(tbuf, &op);	


    }
    assert (tbuf->ptr <= (tbuf->start + tbuf->max_size));
    /*struct timeval gtd1, gtd2;
    gettimeofday(&gtd1, NULL);*/
    tob_process(tbuf, shad, NULL);
    /*gettimeofday(&gtd2, NULL);
    printf ("add_taint @ 0x%lx, %d bytes\n", cpu_get_phys_addr(env, addr),
        length);
    printf ("time required: %f seconds\n",
	    ((float)(gtd2.tv_sec - gtd1.tv_sec)) +
	    ((float)(gtd2.tv_usec - gtd1.tv_usec)) / 1000000.0);*/
    
   count += length;
}

/*
 * These memory callbacks are only for whole-system mode.  User-mode memory
 * accesses are captured by IR instrumentation.
 */
int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    log_dynval(dynval_buffer, ADDRENTRY, STORE, addr);
    return 0;
}

int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf){
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    log_dynval(dynval_buffer, ADDRENTRY, LOAD, addr);
    return 0;
}

namespace llvm {

static void llvm_init(){
    ExecutionEngine *ee = tcg_llvm_ctx->getExecutionEngine();
    FunctionPassManager *fpm = tcg_llvm_ctx->getFunctionPassManager();
    Module *mod = tcg_llvm_ctx->getModule();
    LLVMContext &ctx = mod->getContext();

    // Link logging function in with JIT
    Function *logFunc;
    std::vector<Type*> argTypes;
    // DynValBuffer*
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(uintptr_t)));
    // DynValEntryType
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(DynValEntryType)));
    // LogOp
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(LogOp)));
    // Dynamic value
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(uintptr_t)));
    logFunc = Function::Create(
            FunctionType::get(Type::getVoidTy(ctx), argTypes, false),
            Function::ExternalLinkage, "log_dynval", mod);
    logFunc->addFnAttr(Attribute::AlwaysInline);
    ee->addGlobalMapping(logFunc, (void*) &log_dynval);

    // Create instrumentation pass and add to function pass manager
    llvm::FunctionPass *instfp = createPandaInstrFunctionPass(mod);
    fpm->add(instfp);
    PIFP = static_cast<PandaInstrFunctionPass*>(instfp);
}

} // namespace llvm

void enable_taint(){
    panda_cb pcb;
    pcb.before_block_exec = before_block_exec;
    panda_register_callback(plugin_ptr, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);
    pcb.after_block_exec = after_block_exec;
    panda_register_callback(plugin_ptr, PANDA_CB_AFTER_BLOCK_EXEC, pcb);
    pcb.phys_mem_read = phys_mem_read_callback;
    panda_register_callback(plugin_ptr, PANDA_CB_PHYS_MEM_READ, pcb);
    pcb.phys_mem_write = phys_mem_write_callback;
    panda_register_callback(plugin_ptr, PANDA_CB_PHYS_MEM_WRITE, pcb);
    pcb.cb_cpu_restore_state = cb_cpu_restore_state;
    panda_register_callback(plugin_ptr, PANDA_CB_CPU_RESTORE_STATE, pcb);

    // for hd and network taint
#ifdef CONFIG_SOFTMMU
    pcb.replay_hd_transfer = cb_replay_hd_transfer_taint;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_HD_TRANSFER, pcb);
    if (use_network_taint){
        pcb.replay_net_transfer = cb_replay_net_transfer_taint;
        panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_NET_TRANSFER, pcb);
    }
    pcb.replay_before_cpu_physical_mem_rw_ram = cb_replay_cpu_physical_mem_rw_ram;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_BEFORE_CPU_PHYSICAL_MEM_RW_RAM, pcb);
#endif

    if (!execute_llvm){
        panda_enable_llvm();
    }
    llvm::llvm_init();
    panda_enable_llvm_helpers();

    /*
     * Run instrumentation pass over all helper functions that are now in the
     * module, and verify module.
     */
    llvm::Module *mod = tcg_llvm_ctx->getModule();
    for (llvm::Module::iterator i = mod->begin(); i != mod->end(); i++){
        if (i->isDeclaration()){
            continue;
        }
        PIFP->runOnFunction(*i);
    }
    std::string err;
    if(verifyModule(*mod, llvm::AbortProcessAction, &err)){
        printf("%s\n", err.c_str());
        exit(1);
    }

    /*
     * Taint processor initialization
     */

    //uint32_t ram_size = 536870912; // 500MB each
#ifdef TARGET_X86_64
    // this is only for the fast bitmap which we currently aren't using for
    // 64-bit, it only supports 32-bit
    //XXX FIXME
    uint64_t ram_size = 0;
#else
    uint32_t ram_size = 0xffffffff; //guest address space -- QEMU user mode
#endif
    uint64_t hd_size =  536870912;
    uint64_t io_size = 536870912;
    uint16_t num_vals = 2000; // LLVM virtual registers //XXX assert this
    shadow = tp_init(hd_size, ram_size, io_size, num_vals);
    if (shadow == NULL){
        printf("Error initializing shadow memory...\n");
        exit(1);
    }

    taintfpm = new llvm::FunctionPassManager(tcg_llvm_ctx->getModule());

    // Add the taint analysis pass to our taint pass manager
    llvm::FunctionPass *taintfp =
        llvm::createPandaTaintFunctionPass(15*1048576/* global taint op buffer
        size, 10MB */, NULL /* existing taint cache */);
    PTFP = static_cast<llvm::PandaTaintFunctionPass*>(taintfp);
    taintfpm->add(taintfp);
    taintfpm->doInitialization();

    // Populate taint cache with helper function taint ops
    for (llvm::Module::iterator i = mod->begin(); i != mod->end(); i++){
        if (i->isDeclaration()){
            continue;
        }
        PTFP->runOnFunction(*i);
    }
}

// Derive taint ops
int before_block_exec(CPUState *env, TranslationBlock *tb){
    //printf("%s\n", tcg_llvm_get_func_name(tb));

    if (taintEnabled){
        // process taint ops in io thread taint op buffer
        // NB: we don't need a dynval buffer here.
        tob_process(tob_io_thread, shadow, NULL);
        tob_clear(tob_io_thread);

        taintfpm->run(*(tb->llvm_function));
        DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
        clear_dynval_buffer(dynval_buffer);
    }
    return 0;
}

// Execute taint ops
int after_block_exec(CPUState *env, TranslationBlock *tb,
        TranslationBlock *next_tb){
    if (taintJustEnabled){
        // need to wait until the next TB to start executing taint ops
        taintJustEnabled = false;
        return 0;
    }
    if (taintJustDisabled){
        taintJustDisabled = false;
        execute_llvm = 0;
        generate_llvm = 0;
        panda_do_flush_tb();
        panda_disable_memcb();
        return 0;
    }
    if (taintEnabled){
        DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
        rewind_dynval_buffer(dynval_buffer);

        //printf("%s\n", tb->llvm_function->getName().str().c_str());
        //PTFP->debugTaintOps();
        //printf("\n\n");
        execute_taint_ops(PTFP->ttb, shadow, dynval_buffer);

        // Make sure there's nothing left in the buffer
        assert(dynval_buffer->ptr - dynval_buffer->start == dynval_buffer->cur_size);
    }
    return 0;
}

#ifdef CONFIG_SOFTMMU
// this is for much of the hd taint transfers.
// this gets called from rr_log.c, rr_replay_skipped_calls, RR_CALL_HD_TRANSFER
// case.
int cb_replay_hd_transfer_taint(CPUState *env, uint32_t type, uint64_t src_addr,
        uint64_t dest_addr, uint32_t num_bytes) {
    // Replay hd transfer as taint transfer
    if (taintEnabled) {
        TaintOp top;
        top.typ = BULKCOPYOP;
        top.val.bulkcopy.l = num_bytes;
        switch (type) {
            case HD_TRANSFER_HD_TO_IOB:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_HD_TO_IOB\n");
#endif
                top.val.bulkcopy.a = make_haddr(src_addr);
                top.val.bulkcopy.b = make_iaddr(dest_addr);
                break;
            case HD_TRANSFER_IOB_TO_HD:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_IOB_TO_HD\n");
#endif
                top.val.bulkcopy.a = make_iaddr(src_addr);
                top.val.bulkcopy.b = make_haddr(dest_addr);
                break;
            case HD_TRANSFER_PORT_TO_IOB:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_PORT_TO_IOB\n");
#endif
                top.val.bulkcopy.a = make_paddr(src_addr);
                top.val.bulkcopy.b = make_iaddr(dest_addr);
                break;
            case HD_TRANSFER_IOB_TO_PORT:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_IOB_TO_PORT\n");
#endif
                top.val.bulkcopy.a = make_iaddr(src_addr);
                top.val.bulkcopy.b = make_paddr(dest_addr);
                break;
            case HD_TRANSFER_HD_TO_RAM:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_HD_TO_RAM\n");
                printf("\tSource: 0x%lx, Dest: 0x%lx, Len: %d\n",
                    src_addr, dest_addr, num_bytes);
#endif
                top.val.bulkcopy.a = make_haddr(src_addr);
                top.val.bulkcopy.b = make_maddr(dest_addr);
                break;
            case HD_TRANSFER_RAM_TO_HD:
#ifdef TAINTDEBUG
                printf("replay_hd_transfer HD_TRANSFER_RAM_TO_HD\n");
                printf("\tSource: 0x%lx, Dest: 0x%lx, Len: %d\n",
                    src_addr, dest_addr, num_bytes);
#endif
                top.val.bulkcopy.a = make_maddr(src_addr);
                top.val.bulkcopy.b = make_haddr(dest_addr);
                break;
            default:
                printf ("Impossible hd transfer type: %d\n", type);
                assert (1==0);
        }
        // make the taint op buffer bigger if necessary
        tob_resize(&tob_io_thread);
        // add bulk copy corresponding to this hd transfer to buffer
        // of taint ops for io thread.
        tob_op_write(tob_io_thread, &top);
    }
    return 0;
}

int handle_packet(CPUState *env, uint8_t *buf, int size, uint8_t direction,
        uint64_t old_buf_addr){
    switch (direction){
        case PANDA_NET_RX:
        {
#ifdef TAINTDEBUG
            printf("RX packet\n");
            printf("Buf: 0x%lx, Old Buf: 0x%lx, Size %d\n",
                (uint64_t)buf, old_buf_addr, size);
#endif
            if (taint_label_incoming_network_traffic){
                if (!taintEnabled){
                    printf("Taint plugin: Label operation detected (network)\n");
                    printf("Enabling taint processing\n");
                    taintJustEnabled = true;
                    taintEnabled = true;
                    enable_taint();
                }
                
                Addr a = make_iaddr(old_buf_addr);
                struct taint_op_struct op = {};
                op.typ = LABELOP;
                for (int i = 0; i < size; i++){
                    a.val.ia = old_buf_addr + i;
                    op.val.label.a = a;
                    op.val.label.l = i + count; // byte label
                    //op.val.label.l = 1; // binary label
                    // make the taint op buffer bigger if necessary
                    tob_resize(&tob_io_thread);
                    tob_op_write(tob_io_thread, &op);	
                }
                count += size;
                break;
            }
        }
        case PANDA_NET_TX:
#ifdef TAINTDEBUG
            printf("TX packet\n");
            printf("Buf: 0x%lx, Old Buf: 0x%lx, Size %d\n",
                (uint64_t)buf, old_buf_addr, size);
#endif
            if (taintEnabled && taint_query_outgoing_network_traffic){
                TaintOp top;
                top.typ = QUERYOP;
                top.val.query.l = size;
                top.val.query.a = make_iaddr(old_buf_addr);
                // make the taint op buffer bigger if necessary
                tob_resize(&tob_io_thread);
                tob_op_write(tob_io_thread, &top);
            }
            break;
        default:
            assert(0);
    }
    return 0;
}

// this is for much of the network taint transfers.
// this gets called from rr_log.c, rr_replay_skipped_calls, RR_CALL_NET_TRANSFER
// case.
int cb_replay_net_transfer_taint(CPUState *env, uint32_t type, uint64_t src_addr,
        uint64_t dest_addr, uint32_t num_bytes){
    // Replay network transfer as taint transfer
    if (taintEnabled) {
        TaintOp top;
        top.typ = BULKCOPYOP;
        top.val.bulkcopy.l = num_bytes;
        switch (type) {
            case NET_TRANSFER_RAM_TO_IOB:
#ifdef TAINTDEBUG
                printf("NET_TRANSFER_RAM_TO_IOB src: 0x%lx, dest 0x%lx, len %d\n",
                    src_addr, dest_addr, num_bytes);
#endif
                top.val.bulkcopy.a = make_maddr(src_addr);
                top.val.bulkcopy.b = make_iaddr(dest_addr);
                break;
            case NET_TRANSFER_IOB_TO_RAM:
#ifdef TAINTDEBUG
                printf("NET_TRANSFER_IOB_TO_RAM src: 0x%lx, dest 0x%lx, len %d\n",
                    src_addr, dest_addr, num_bytes);
#endif
                top.val.bulkcopy.a = make_iaddr(src_addr);
                top.val.bulkcopy.b = make_maddr(dest_addr);
                break;
            case NET_TRANSFER_IOB_TO_IOB:
#ifdef TAINTDEBUG
                printf("NET_TRANSFER_IOB_TO_IOB src: 0x%lx, dest 0x%lx, len %d\n",
                    src_addr, dest_addr, num_bytes);
#endif
                top.val.bulkcopy.a = make_iaddr(src_addr);
                top.val.bulkcopy.b = make_iaddr(dest_addr);
                break;
            default:
                assert(0);
        }
        // make the taint op buffer bigger if necessary
        tob_resize(&tob_io_thread);
        // add bulk copy corresponding to this hd transfer to buffer
        // of taint ops for io thread.
        tob_op_write(tob_io_thread, &top);
    }
    return 0;
}

// this does a bunch of the dmas in hd taint transfer
int cb_replay_cpu_physical_mem_rw_ram(CPUState *env, uint32_t is_write,
        uint8_t *src_addr, uint64_t dest_addr, uint32_t num_bytes){
    // NB:
    // is_write == 1 means write from qemu buffer to guest RAM.
    // is_write == 0 means RAM -> qemu buffer
    // Replay dmas in hd taint transfer
    if (taintEnabled) {
        TaintOp top;
        top.typ = BULKCOPYOP;
        top.val.bulkcopy.l = num_bytes;
        if (is_write) {
            top.val.bulkcopy.a = make_iaddr((uint64_t)src_addr);
            top.val.bulkcopy.b = make_maddr(dest_addr);
        }
        else {
            top.val.bulkcopy.a = make_maddr(dest_addr);
            top.val.bulkcopy.b = make_iaddr((uint64_t)src_addr);
        }
        // make the taint op buffer bigger if necessary
        tob_resize(&tob_io_thread);
        // add bulk copy corresponding to this hd transfer to buffer
        // of taint ops for io thread.
        tob_op_write(tob_io_thread, &top);
    }
    return 0;
}
#endif


int cb_cpu_restore_state(CPUState *env, TranslationBlock *tb){
    if (taintEnabled){
        //printf("EXCEPTION - logging\n");
        DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
        log_exception(dynval_buffer);

        // Then execute taint ops up until the exception occurs.  Execution of taint
        // ops will stop at the point of the exception.
        rewind_dynval_buffer(dynval_buffer);
        execute_taint_ops(PTFP->ttb, shadow, dynval_buffer);

        // Make sure there's nothing left in the buffer
        assert(dynval_buffer->ptr - dynval_buffer->start == dynval_buffer->cur_size);
    }
    return 0;
}

#if 0 // old version
int guest_hypercall_callback(CPUState *env){
#ifdef TARGET_I386
    if (env->regs[R_EAX] == 0xdeadbeef){
        target_ulong buf_start = env->regs[R_ECX];
        target_ulong buf_len = env->regs[R_EDX];

        if (env->regs[R_EBX] == 0){ //Taint label
            if (!taintEnabled){
                printf("Taint plugin: Label operation detected\n");
                printf("Enabling taint processing\n");
                taintJustEnabled = true;
                taintEnabled = true;
                enable_taint();
            }

            TaintOpBuffer *tempBuf = tob_new(500*1048576 /* 5MB */);
#ifndef CONFIG_SOFTMMU
            add_taint(shadow, tempBuf, (uint64_t)buf_start, (int)buf_len);
#else
            add_taint(shadow, tempBuf, cpu_get_phys_addr(env, buf_start),
                (int)buf_len);
#endif //CONFIG_SOFTMMU
            tob_delete(tempBuf);
        }

        else if (env->regs[R_EBX] == 1){ //Query taint on label
#ifndef CONFIG_SOFTMMU
            bufplot(shadow, (uint64_t)buf_start, (int)buf_len);
#else
            bufplot(shadow, cpu_get_phys_addr(env, buf_start), (int)buf_len);
#endif //CONFIG_SOFTMMU
            printf("Taint plugin: Query operation detected\n");
            printf("Disabling taint processing\n");
            taintEnabled = false;
            taintJustDisabled = true;
        }
    }
#endif // TARGET_I386
    return 1;
}
#endif

// XXX: Support all features of label and query program
int guest_hypercall_callback(CPUState *env){
#ifdef TARGET_I386
    target_ulong buf_start = env->regs[R_EBX];
    target_ulong buf_len = env->regs[R_ECX];

    // call to iferret to label data
    // EBX contains addr of that data
    // ECX contains size of data
    // EDI is a pointer to a buffer containing the label string
    // ESI contains the length of that label
    // EDX = starting offset (for positional labels only)

    if (env->regs[R_EAX] == 7 || env->regs[R_EAX] == 8){
        if (!taintEnabled){
            printf("Taint plugin: Label operation detected\n");
            printf("Enabling taint processing\n");
            taintJustEnabled = true;
            taintEnabled = true;
            enable_taint();
        }


        TaintOpBuffer *tempBuf = tob_new( buf_len * sizeof(TaintOp));
	
	
	add_taint(env, shadow, tempBuf, (uint64_t)buf_start, (int)buf_len);
		//add_taint(env, shadow, tempBuf, (uint64_t)buf_start, 4);
        tob_delete(tempBuf);

    }    

    //mz Query taint on this buffer
    //mz EBX = start of buffer (VA)
    //mz ECX = size of buffer (bytes)
    // EDI is a pointer to a buffer containing the filename or another name for this query
    // ESI contains the length of that string
    // EDX = starting offset - for file queries
    else if (env->regs[R_EAX] == 9){ //Query taint on label
        if (taintEnabled){
            printf("Taint plugin: Query operation detected\n");
            Addr a = make_maddr(buf_start);
            bufplot(env, shadow, &a, (int)buf_len);
        }
        //printf("Disabling taint processing\n");
        //taintEnabled = false;
        //taintJustDisabled = true;
        //printf("Label occurrences on HD: %d\n", shad_dir_occ_64(shadow->hd));
    }
#endif // TARGET_I386
    return 1;
}

#ifndef CONFIG_SOFTMMU

// Globals to keep track of file descriptors
int infd = -1;
int outfd = -1;

/*
 * Kind of a hacky way to see if the file being opened is something we're
 * interested in.  For now, we are working under the assumption that a program
 * will open/read one file of interest, and open/write the other file of
 * interest.  So we assume that files that are opened from /etc and /lib aren't
 * of interest. /proc and openssl.cnf also aren't interesting, from looking at
 * openssl.
 */
static int user_open(bitmask_transtbl *fcntl_flags_tbl, abi_long ret, void *p,
              abi_long flagarg){
    const char *file = path((const char*)p);
    unsigned int flags = target_to_host_bitmask(flagarg, fcntl_flags_tbl);
    if (ret > 0){
        if((strncmp(file, "/etc", 4) != 0)
                && (strncmp(file, "/lib", 4) != 0)
                && (strncmp(file, "/proc", 5) != 0)
                //&& (strncmp(file, "/dev", 4) != 0)
                && (strncmp(file, "/usr", 4) != 0)
                && (strstr(file, "openssl.cnf") == 0)
                && (strstr(file, "xpdfrc") == 0)){
            printf("open %s for ", file);
            if ((flags & (O_RDONLY | O_WRONLY)) == O_RDONLY){
                printf("read\n");
                infd = ret;
            }
            if (flags & O_WRONLY){
                printf("write\n");
                outfd = ret;
            }
        }
    }
    return 0;
}

static int user_creat(abi_long ret, void *p){
    const char *file = path((const char*)p);
    if (ret > 0){
        printf("open %s for write\n", file);
        outfd = ret;
    }
    return 0;
}

static int user_read(CPUState *env, abi_long ret, abi_long fd, void *p){
    if (ret > 0 && fd == infd){
        TaintOpBuffer *tempBuf = tob_new(5*1048576 /* 1MB */);
        add_taint(env, shadow, tempBuf, (uint64_t)p /*pointer*/, ret /*length*/);
        tob_delete(tempBuf);
    }
    return 0;
}

static int user_write(CPUState *env, abi_long ret, abi_long fd, void *p){
    if (ret > 0 && fd == outfd){
        Addr a = make_maddr((uint64_t)p);
        bufplot(env, shadow, &a /*pointer*/, ret /*length*/);
    }
    return 0;
}

int user_after_syscall(void *cpu_env, bitmask_transtbl *fcntl_flags_tbl,
                       int num, abi_long arg1, abi_long arg2, abi_long arg3,
                       abi_long arg4, abi_long arg5, abi_long arg6,
                       abi_long arg7, abi_long arg8, void *p, abi_long ret){
    CPUState *env = (CPUState *)cpu_env;
    switch (num){
        case TARGET_NR_read:
            user_read(env, ret, arg1, p);
            break;
        case TARGET_NR_write:
            user_write(env, ret, arg1, p);
            break;
        case TARGET_NR_open:
            user_open(fcntl_flags_tbl, ret, p, arg2);
            break;
        case TARGET_NR_openat:
            user_open(fcntl_flags_tbl, ret, p, arg3);
            break;
        case TARGET_NR_creat:
            user_creat(ret, p);
            break;
        default:
            break;
    }
    return 0;
}

#endif // CONFIG_SOFTMMU



bool init_plugin(void *self) {
    printf("Initializing taint plugin\n");
    plugin_ptr = self;
    panda_cb pcb;
    panda_enable_memcb();
    panda_disable_tb_chaining();
    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);
    if (use_network_taint){
        pcb.replay_handle_packet = handle_packet;
        panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_HANDLE_PACKET, pcb);
    }
#ifndef CONFIG_SOFTMMU
    pcb.user_after_syscall = user_after_syscall;
    panda_register_callback(self, PANDA_CB_USER_AFTER_SYSCALL, pcb);
#endif

    tob_io_thread = tob_new(tob_io_thread_max_size);


    int i;
    for (i = 0; i < panda_argc; i++) {
      if (0 == strncmp(panda_argv[i], "max_taintset_card", 17)) {
	// Format is sample:key=value                                                                                                                                                                                 
	char *str = strchr(panda_argv[i], '=');
	if (str) {
	  max_taintset_card = atoi(str+1);
	  printf ("max_taintset_card = %d\n", max_taintset_card);
	}
      }
      if (0 == strncmp(panda_argv[i], "max_taintset_compute_number", 24)) {
	// Format is sample:key=value                                                                                                                                                                                 
	char *str = strchr(panda_argv[i], '=');
	if (str) {
	  max_taintset_compute_number = atoi(str+1);
	  printf ("max_taintset_card = %d\n", max_taintset_compute_number);
	}
      }
    }


    return true;
}

void uninit_plugin(void *self) {
    /*
     * XXX: Here, we unload our pass from the PassRegistry.  This seems to work
     * fine, until we reload this plugin again into QEMU and we get an LLVM
     * assertion saying the pass is already registered.  This seems like a bug
     * with LLVM.  Switching between TCG and LLVM works fine when passes aren't
     * added to LLVM.
     */
    llvm::PassRegistry *pr = llvm::PassRegistry::getPassRegistry();
    const llvm::PassInfo *pi =
        //pr->getPassInfo(&llvm::PandaInstrFunctionPass::ID);
        pr->getPassInfo(llvm::StringRef("PandaInstr"));
    if (!pi){
        printf("Unable to find 'PandaInstr' pass in pass registry\n");
    }
    else {
        pr->unregisterPass(*pi);
    }

    if (taintfpm) delete taintfpm; // Delete function pass manager and pass
    if (shadow) tp_free(shadow);
    if (tob_io_thread) tob_delete(tob_io_thread);

    panda_disable_llvm();
    panda_disable_memcb();
    panda_enable_tb_chaining();
}

