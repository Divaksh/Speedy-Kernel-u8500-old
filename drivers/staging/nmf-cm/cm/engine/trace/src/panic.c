/*
 * Copyright (C) ST-Ericsson SA 2010
 * Author: Jean-Philippe FASSINO <jean-philippe.fassino@stericsson.com> for ST-Ericsson.
 * License terms: GNU General Public License (GPL) version 2.
 */
#include <cm/inc/cm_type.h>
#include <cm/engine/component/inc/introspection.h>
#include <cm/engine/component/inc/bind.h>
#include <cm/engine/executive_engine_mgt/inc/executive_engine_mgt.h>
#include <cm/engine/trace/inc/trace.h>
#include <cm/engine/api/control/irq_engine.h>

#include <cm/engine/utils/inc/convert.h>
#include <share/communication/inc/nmf_service.h>

PUBLIC t_cm_error cm_SRV_allocateTraceBufferMemory(t_nmf_core_id coreId, t_cm_domain_id domainId)
{
    t_ee_state *state = cm_EEM_getExecutiveEngine(coreId);

    state->traceDataHandle = cm_DM_Alloc(domainId, SDRAM_EXT16,
            TRACE_BUFFER_SIZE * sizeof(struct t_nmf_trace) / 2, CM_MM_ALIGN_WORD, TRUE);
    if (state->traceDataHandle == INVALID_MEMORY_HANDLE)
        return CM_NO_MORE_MEMORY;
    else
    {
        t_uint32 mmdspAddr;
        int i;

        state->traceDataAddr = (struct t_nmf_trace*)cm_DSP_GetHostLogicalAddress(state->traceDataHandle);
        cm_DSP_GetDspAddress(state->traceDataHandle, &mmdspAddr);
        cm_writeAttribute(state->instance, "rtos/commonpart/traceDataAddr", mmdspAddr);

        eeState[coreId].readTracePointer = 0;
        eeState[coreId].lastReadedTraceRevision = 0;

        for(i = 0; i < TRACE_BUFFER_SIZE; i++)
            state->traceDataAddr[i].revision = 0;

        return CM_OK;
    }
}

PUBLIC void cm_SRV_deallocateTraceBufferMemory(t_nmf_core_id coreId)
{
    t_ee_state *state = cm_EEM_getExecutiveEngine(coreId);

    state->traceDataAddr = 0;
    cm_DM_Free(state->traceDataHandle, TRUE);
}

static t_uint32 swapHalfWord(t_uint32 word)
{
    return (word >> 16) | (word << 16);
}

PUBLIC EXPORT_SHARED t_cm_trace_type CM_ENGINE_GetNextTrace(
        t_nmf_core_id               coreId,
        struct t_nmf_trace          *trace)
{
    t_ee_state *state = cm_EEM_getExecutiveEngine(coreId);
    t_uint32 foundRevision;
    t_cm_trace_type type;

    OSAL_LOCK_API();
    if (state->traceDataAddr == NULL) {
        type = CM_MPC_TRACE_NONE;
	goto out;
    }

    foundRevision = swapHalfWord(state->traceDataAddr[state->readTracePointer].revision);

    if(foundRevision <= state->lastReadedTraceRevision)
    {
        // It's an old trace forgot it
        type = CM_MPC_TRACE_NONE;
    }
    else
    {
        struct t_nmf_trace          *traceRaw;

        if(foundRevision == state->lastReadedTraceRevision + 1)
        {
            type = CM_MPC_TRACE_READ;
        }
        else
        {
            type = CM_MPC_TRACE_READ_OVERRUN;
            /*
             * If we find that revision is bigger, thus we are in overrun, then we take the writePointer + 1 which
             * correspond to the older one.
             * => Here there is a window where the MMDSP could update writePointer just after
             */
            state->readTracePointer = (cm_readAttributeNoError(state->instance, "rtos/commonpart/writePointer") + 1) % TRACE_BUFFER_SIZE;
        }

        traceRaw = &state->traceDataAddr[state->readTracePointer];

        trace->timeStamp = swapHalfWord(traceRaw->timeStamp);
        trace->componentId = swapHalfWord(traceRaw->componentId);
        trace->traceId = swapHalfWord(traceRaw->traceId);
        trace->paramOpt = swapHalfWord(traceRaw->paramOpt);
        trace->componentHandle = swapHalfWord(traceRaw->componentHandle);
        trace->parentHandle = swapHalfWord(traceRaw->parentHandle);

        trace->params[0] = swapHalfWord(traceRaw->params[0]);
        trace->params[1] = swapHalfWord(traceRaw->params[1]);
        trace->params[2] = swapHalfWord(traceRaw->params[2]);
        trace->params[3] = swapHalfWord(traceRaw->params[3]);

        state->readTracePointer = (state->readTracePointer + 1) % TRACE_BUFFER_SIZE;
        state->lastReadedTraceRevision = swapHalfWord(traceRaw->revision);
        trace->revision = state->lastReadedTraceRevision;
    }

out:
    OSAL_UNLOCK_API();

    return type;
}


/*
 * Panic
 */
const struct {
	char* name;
    unsigned int info1:1;
    unsigned int PC:1;
	unsigned int SP:1;
	unsigned int interface:1;
} reason_descrs[] = {
		{"NONE_PANIC",                0,  0,  0,  0},
		{"INTERNAL_PANIC",            1,  0,  0,  0},
		{"MPC_NOT_RESPONDING_PANIC",  0,  0,  0,  0}, /* Should not be useful since in that case CM_getServiceDescription() not call */
		{"USER_STACK_OVERFLOW",       0,  1,  1,  0},
		{"SYSTEM_STACK_OVERFLOW",     0,  1,  1,  0},
		{"UNALIGNED_LONG_ACCESS",     0,  1,  0,  0},
		{"EVENT_FIFO_OVERFLOW",       0,  0,  0,  1},
		{"PARAM_FIFO_OVERFLOW",       0,  0,  0,  1},
		{"INTERFACE_NOT_BINDED",      0,  1,  0,  0},
		{"USER_PANIC",                1,  0,  0,  0}
};

static t_component_instance* getCorrespondingInstance(
        t_panic_reason panicReason,
        t_uint32 panicThis,
        t_dup_char *itfName,
	t_cm_instance_handle *instHandle) {
    t_component_instance *instance;
    t_uint32 k;

    for (k=0; k<ComponentTable.idxMax; k++) {
        if ((instance = componentEntry(k)) == NULL)
            continue;
        if(panicReason == PARAM_FIFO_OVERFLOW ||
                panicReason == EVENT_FIFO_OVERFLOW) {
            // Panic has been generated by binding component, search the client who has call it
            // and return the client handle (not the BC one).
            int i;

            if(instance->thisAddress == panicThis && panicThis == 0) {
                *itfName = "Internal NMF service";
                *instHandle = ENTRY2HANDLE(instance, k);
                return instance;
            }

            for(i = 0; i < instance->Template->requireNumber; i++) {
                int nb = instance->Template->requires[i].collectionSize, j;
                for(j = 0; j < nb; j++) {
                    if(instance->interfaceReferences[i][j].instance != NULL &&
                            instance->interfaceReferences[i][j].instance != (t_component_instance *)NMF_HOST_COMPONENT &&
                            instance->interfaceReferences[i][j].instance != (t_component_instance *)NMF_VOID_COMPONENT &&
                            instance->interfaceReferences[i][j].instance->thisAddress == panicThis)
                    {
                        *itfName = instance->Template->requires[i].name;
                        *instHandle = ENTRY2HANDLE(instance, k);
                        return instance;
                    }
                }
            }
        } else {
            // The component which has generated the panic is the good one.

            if(instance->thisAddress == panicThis) {
		*itfName = "?";
                *instHandle = ENTRY2HANDLE(instance, k);
                return instance;
            }
        }
    }

    *itfName = "?";
    *instHandle = 0;
    return 0;
}

PUBLIC EXPORT_SHARED t_cm_error CM_ReadMPCString(
        t_nmf_core_id               coreId,
        t_uint32                    dspAddress,
        char *                      buffer,
        t_uint32                    bufferSize) {

    while(--bufferSize > 0)
    {
        char ch = cm_DSP_ReadXRamWord(coreId, dspAddress++);
        if(ch == 0)
            break;

        *buffer++ = ch;
    };

    *buffer = 0;

    // Reset panicReason
    cm_writeAttribute(cm_EEM_getExecutiveEngine(coreId)->instance,
            "rtos/commonpart/serviceReason", MPC_SERVICE_NONE);

    return CM_OK;
}

/****************/
/* Generic part */
/****************/
PUBLIC EXPORT_SHARED t_cm_error CM_getServiceDescription(
        t_nmf_core_id               coreId,
        t_cm_service_type           *srcType,
        t_cm_service_description    *srcDescr)
{
    t_uint32 serviceReason;
    t_component_instance *ee;

    // Acknowledge interrupt (do it before resetting panicReason)
    cm_DSP_AcknowledgeDspIrq(coreId, DSP2ARM_IRQ_1);

    ee = cm_EEM_getExecutiveEngine(coreId)->instance;

    // Read panicReason
    serviceReason = cm_readAttributeNoError(ee, "rtos/commonpart/serviceReason");
    if(serviceReason == MPC_SERVICE_PRINT)
    {
        *srcType = CM_MPC_SERVICE_PRINT;

        srcDescr->u.print.dspAddress = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo0");
        srcDescr->u.print.value1 = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo1");
        srcDescr->u.print.value2 = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo2");
    }
    else if(serviceReason == MPC_SERVICE_TRACE)
    {
        *srcType = CM_MPC_SERVICE_TRACE;
    }
    else if(serviceReason != MPC_SERVICE_NONE)
    {
        t_uint32 panicThis;
        t_dup_char itfName;
        t_component_instance *instance;

        *srcType = CM_MPC_SERVICE_PANIC;
        srcDescr->u.panic.panicReason = (t_panic_reason)serviceReason;
        srcDescr->u.panic.panicSource = MPC_EE;
        srcDescr->u.panic.info.mpc.coreid = coreId;

        // Read panicThis
        panicThis = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo0");

        instance = getCorrespondingInstance(srcDescr->u.panic.panicReason, panicThis, &itfName, &srcDescr->u.panic.info.mpc.faultingComponent);

        LOG_INTERNAL(0, "Error: Panic(%s, %s), This=%x", cm_getDspName(coreId),
                reason_descrs[srcDescr->u.panic.panicReason].name, (void*)panicThis, 0, 0, 0);

        if(reason_descrs[srcDescr->u.panic.panicReason].interface != 0)
        {
            LOG_INTERNAL(0, ", interface=%s", itfName, 0, 0, 0, 0, 0);
        }

        if(reason_descrs[srcDescr->u.panic.panicReason].info1 != 0)
        {
            // Info 1
            srcDescr->u.panic.info.mpc.panicInfo1 = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo1");

            LOG_INTERNAL(0, ", Info=%x", srcDescr->u.panic.info.mpc.panicInfo1, 0, 0, 0, 0, 0);
        }

        if(reason_descrs[srcDescr->u.panic.panicReason].PC != 0)
        {
            t_uint32 DspAddress = 0xFFFFFFFF;
            t_uint32 DspSize = 0x0;

            // PC need to be read in rtos/commonpart/serviceInfo1
            srcDescr->u.panic.info.mpc.panicInfo1 = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo1");

            if(instance != 0)
            {
                cm_DSP_GetDspAddress(instance->memories[instance->Template->codeMemory->id], &DspAddress);
                cm_DSP_GetDspMemoryHandleSize(instance->memories[instance->Template->codeMemory->id], &DspSize);
            }

            if(DspAddress <= srcDescr->u.panic.info.mpc.panicInfo1 &&
                    srcDescr->u.panic.info.mpc.panicInfo1 < (DspAddress + DspSize))
                LOG_INTERNAL(0, ", PC:off=%x <abs=%x>",
                        srcDescr->u.panic.info.mpc.panicInfo1 - DspAddress,
                        srcDescr->u.panic.info.mpc.panicInfo1, 0, 0, 0, 0);
            else
                LOG_INTERNAL(0, ", PC:<abs=%x>", srcDescr->u.panic.info.mpc.panicInfo1, 0, 0, 0, 0, 0);
        }

        if(reason_descrs[srcDescr->u.panic.panicReason].SP != 0)
        {
            srcDescr->u.panic.info.mpc.panicInfo2 = cm_readAttributeNoError(ee, "rtos/commonpart/serviceInfo2");

            LOG_INTERNAL(0, ", SP=%x", srcDescr->u.panic.info.mpc.panicInfo2, 0, 0, 0, 0, 0);
        }

        LOG_INTERNAL(0, "\n", 0, 0, 0, 0, 0, 0);

        if(instance != 0)
        {
            LOG_INTERNAL(0, "Error:  Component=%s<%s>\n",
                    instance->pathname, instance->Template->name, 0, 0, 0, 0);
        }

        // We don't set rtos/commonpart/serviceReason = MPC_SERVICE_NONE, since we don't want the
        // MMDSP to continue execution, and we put in in Panic state
        cm_DSP_SetStatePanic(coreId);
    }
    else
    {
        *srcType = CM_MPC_SERVICE_NONE;
    }

    return CM_OK;
}
