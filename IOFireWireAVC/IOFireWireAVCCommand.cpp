/*
 * Copyright (c) 1998-2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <IOKit/avc/IOFireWireAVCCommand.h>
#include <IOKit/avc/IOFireWireAVCConsts.h>

#include <IOKit/firewire/IOFireWireNub.h>
#include <IOKit/firewire/IOFireWireController.h>
#include <IOKit/IOSyncer.h>

#if FIRELOG
#import <IOKit/firewire/IOFireLog.h>
#define FIRELOG_MSG(x) FireLog x
#else
#define FIRELOG_MSG(x) do {} while (0)
#endif

#define kInterimTimeout 10000000

class IOFireWireAVCCommandInGen : public IOFireWireAVCCommand
{
    OSDeclareDefaultStructors(IOFireWireAVCCommandInGen)
    
protected:
    UInt32 fDummy;
    
    virtual IOReturn	complete(IOReturn status);

public:
    virtual bool init(IOFireWireNub *device, UInt32 generation, const UInt8 * command, UInt32 cmdLen,
                                                    UInt8 * response, UInt32 * responseLen);
    virtual IOReturn reinit(IOFireWireNub *device, UInt32 generation, const UInt8 * command, UInt32 cmdLen,
                                                    UInt8 * response, UInt32 * responseLen);
                                                    
private:
    OSMetaClassDeclareReservedUnused(IOFireWireAVCCommandInGen, 0);
    OSMetaClassDeclareReservedUnused(IOFireWireAVCCommandInGen, 1);
    OSMetaClassDeclareReservedUnused(IOFireWireAVCCommandInGen, 2);
    OSMetaClassDeclareReservedUnused(IOFireWireAVCCommandInGen, 3);
};


OSDefineMetaClassAndStructors(IOFireWireAVCCommand, IOFWCommand)
//OSMetaClassDefineReservedUnused(IOFireWireAVCCommand, 0);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommand, 1);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommand, 2);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommand, 3);

/*
 Possible states for an AVCCommand:
 
 Not submitted: fTimeout = 0, fWriteNodeID = kFWBadNodeID, fStatus != Busy().
 Write submitted: fTimeout = 0, fWriteNodeID = kFWBadNodeID, fStatus = kIOReturnBusy
 Write complete: fTimeout = 250000, fWriteNodeID = device node, fStatus = kIOReturnBusy
 Interim received: fTimeout = kInterimTimeout, fWriteNodeID = device node, fStatus = kIOReturnBusy
 Complete: fStatus != Busy()
*/

void IOFireWireAVCCommand::writeDone(void *refcon, IOReturn status, IOFireWireNub *device, IOFWCommand *fwCmd)
{	
    IOFireWireAVCCommand *me = (IOFireWireAVCCommand *)refcon;

    FIRELOG_MSG(("IOFireWireAVCCommand::writeDone (this=0x%08X)\n",me));

	if(status == kIOReturnSuccess) {
        // Store current node and generation
        if(device)
            device->getNodeIDGeneration(me->fWriteGen, me->fWriteNodeID);
        
        // Start timer for command response
        me->fTimeout = 250000;
        me->updateTimer();
    }
    else {
        IOLog("Write for %p done, status %x\n", refcon, status);
        me->complete(status);
    }
}

UInt32 IOFireWireAVCCommand::handleResponse(UInt16 nodeID, UInt32 len, const void *buf)
{
	FIRELOG_MSG(("IOFireWireAVCCommand::handleResponse (this=0x%08X)\n",this));

    const UInt8 *p;
    UInt32 i;
    UInt32 res = kFWResponseAddressError;
    
    // copy the status bytes from fPseudoSpace if this is for us
    // Don't need to check generation because the command is cancelled when a bus reset happens.
    // fTimeout is only non-zero if the write was successful.
    if(fTimeout && nodeID == fWriteNodeID) {
        p = (const UInt8 *)buf;
        if(p[kAVCCommandResponse] == kAVCInterimStatus) {
            fTimeout = kInterimTimeout;	// We could wait for ever after the Interim, 10 seconds seems long enough
            updateTimer();
        }
        else {
            if(len > *fResponseLen)
                len = *fResponseLen;
            for (i = 0 ; i < len ; i++)
                fResponse[i] = *p++;
            *fResponseLen = len;
            // Make sure we don't accept another response, we're done!
            fTimeout = 0;
            complete(kIOReturnSuccess);
        }
        res = kFWResponseComplete;
    }
    else {
        //IOLog("%p: ------ Write not for me ----------\n", this);
        //IOLog("nodeID: %d-%d\n", nodeID, fWriteNodeID);
        //IOLog("Data: %x len %d\n", (unsigned int) *(const UInt32 *)buf, (int)len);
    }
    return res;
}

void IOFireWireAVCCommand::free()
{
	FIRELOG_MSG(("IOFireWireAVCCommand::free (this=0x%08X)\n",this));

    if (fWriteCmd) {
        fWriteCmd->release();
    }
    if(fMem)
        fMem->release();

    IOFWCommand::free();
}

IOReturn IOFireWireAVCCommand::complete(IOReturn state)
{
	FIRELOG_MSG(("IOFireWireAVCCommand::complete (this=0x%08X)\n",this));

    if(state == kIOFireWireBusReset && fWriteNodeID == kFWBadNodeID)
            state = kIOReturnOffline;		// Write was 'retry on bus reset', so device must have gone.

    state = IOFWCommand::complete(state);
    // If write was sent successfully but response wasn't received, or
    // Command was written, bus reset happened before response was sent.
    // - try again

    if( (state == kIOReturnTimeout && fTimeout != 0 && fCurRetries--) ||
            (state == kIOFireWireBusReset)) {
        FWAddress addr;
    
        // setup quad write
        addr.addressHi   = kCSRRegisterSpaceBaseAddressHi;
        addr.addressLo   = kFCPCommandAddress;
        if(fMem)
            ((IOFWWriteCommand *)fWriteCmd)->reinit(addr, fMem, writeDone, this);
        else
            ((IOFWWriteQuadCommand *)fWriteCmd)->reinit(addr,(UInt32 *)fCommand, fCmdLen/4, writeDone, this);
        fTimeout = 0;
        return fStatus = startExecution();
    }
    if(fSync)
        fSyncWakeup->signal(state);
    //else if(fComplete)
	//(*fComplete)(fRefCon, state, fControl, this);
    return state;
}


IOReturn IOFireWireAVCCommand::execute()
{
	FIRELOG_MSG(("IOFireWireAVCCommand::execute (this=0x%08X)\n",this));

    fStatus = kIOReturnBusy;
    fWriteCmd->submit();
    return fStatus;
}

IOReturn IOFireWireAVCCommand::resetInterimTimeout()
{
	FIRELOG_MSG(("IOFireWireAVCCommand::resetInterimTimeout (this=0x%08X)\n",this));

    // Reinitialize the timeout if we're waiting for the final response after an interim.
    // Must check internal state with FireWire command gate closed.
	fControl->closeGate();
	if(fTimeout == kInterimTimeout && fStatus == kIOReturnBusy)
        updateTimer();
	fControl->openGate();

    return kIOReturnSuccess;
}

bool IOFireWireAVCCommand::init(IOFireWireNub *device, const UInt8 * command, UInt32 cmdLen,
                                                    UInt8 * response, UInt32 * responseLen)
{
	FIRELOG_MSG(("IOFireWireAVCCommand::init (this=0x%08X)\n",this));

    FWAddress addr;

    // setup quad write
    addr.addressHi   = kCSRRegisterSpaceBaseAddressHi;
    addr.addressLo   = kFCPCommandAddress;

    IOFWCommand::initWithController(device->getController());
    // Start out without timeout, update when command is accepted by device
    fTimeout = 0;
    fSync = true;
    fCancelOnReset = true;
    fResponse = response;
    fResponseLen = responseLen;
    fWriteNodeID = kFWBadNodeID;
    fMaxRetries = 4;
    fCurRetries = fMaxRetries;
    fCmdLen = cmdLen;
    
    // create command
    if(cmdLen == 4 || cmdLen == 8) {
        fMem = NULL;
        fCommand = command;
        fWriteCmd = device->createWriteQuadCommand(addr,(UInt32 *)command, cmdLen/4, writeDone, this);
        if(!fWriteCmd) {
            return false;
        }
    }
    else {
        fCommand = NULL;
        fMem = IOMemoryDescriptor::withAddress((void *)command, cmdLen,
                    kIODirectionOutIn);
        if(!fMem) {
            return false;
        }

		IOReturn err = fMem->prepare();
		if( err != kIOReturnSuccess )
		{
			fMem->release();
			return false;
		}
		
        fWriteCmd = device->createWriteCommand(addr, fMem, writeDone, this);
        if(!fWriteCmd) {
            return false;
        }
    }
    return true;
}

IOReturn IOFireWireAVCCommand::reinit(IOFireWireNub *device, const UInt8 * command, UInt32 cmdLen,
                                                    UInt8 * response, UInt32 * responseLen)
{
	FIRELOG_MSG(("IOFireWireAVCCommand::reinit (this=0x%08X)\n",this));

    if(Busy())
        return fStatus;
    if(fMem) {
        fMem->release();
        fMem = NULL;
    }
    if(fWriteCmd) {
        fWriteCmd->release();
        fWriteCmd = NULL;
    }
    if(init(device, command, cmdLen, response, responseLen))
        return kIOReturnSuccess;
    else
        return kIOReturnNoMemory;
}
                                                    
IOFireWireAVCCommand *
IOFireWireAVCCommand::withNub(IOFireWireNub *device, const UInt8 * command, UInt32 cmdLen,
                                                    UInt8 * response, UInt32 * responseLen)
{
    IOFireWireAVCCommand *me = new IOFireWireAVCCommand;

	FIRELOG_MSG(("IOFireWireAVCCommand::withNub (this=0x%08X)\n",me));

	if(me) {
        if(!me->init(device, command, cmdLen, response, responseLen)) {
            me->release();
            me = NULL;
        }
    }
    return me;
}
                                                    
IOFireWireAVCCommand *
IOFireWireAVCCommand::withNub(IOFireWireNub *device, UInt32 generation,
            const UInt8 * command, UInt32 cmdLen, UInt8 * response, UInt32 * responseLen)
{

    IOFireWireAVCCommandInGen *me = new IOFireWireAVCCommandInGen;

	FIRELOG_MSG(("IOFireWireAVCCommand::withNub (this=0x%08X)\n",me));


	if(me) {
        if(!me->init(device, generation, command, cmdLen, response, responseLen)) {
            me->release();
            me = NULL;
        }
    }
    return me;
}

/* --------------- IOFireWireAVCCommandInGen -------------------- */
OSDefineMetaClassAndStructors(IOFireWireAVCCommandInGen, IOFireWireAVCCommand)
OSMetaClassDefineReservedUnused(IOFireWireAVCCommandInGen, 0);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommandInGen, 1);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommandInGen, 2);
OSMetaClassDefineReservedUnused(IOFireWireAVCCommandInGen, 3);


IOReturn IOFireWireAVCCommandInGen::complete(IOReturn state)
{
	FIRELOG_MSG(("IOFireWireAVCCommandInGen::complete (this=0x%08X)\n",this));
	
    state = IOFWCommand::complete(state);

    // If write was sent successfully but response wasn't received - try again
    // Don't retry on bus reset!
    if(state == kIOReturnTimeout && fTimeout != 0 && fCurRetries--) {
        FWAddress addr;
    
        // setup quad write
        addr.nodeID		 = fWriteNodeID;
        addr.addressHi   = kCSRRegisterSpaceBaseAddressHi;
        addr.addressLo   = kFCPCommandAddress;
        if(fMem)
            ((IOFWWriteCommand *)fWriteCmd)->reinit(fWriteGen, addr, fMem, writeDone, this);
        else
            ((IOFWWriteQuadCommand *)fWriteCmd)->reinit(fWriteGen, addr,
                                            (UInt32 *)fCommand, fCmdLen/4, writeDone, this);
        fTimeout = 0;
        return fStatus = startExecution();
    }
    if(fSync)
        fSyncWakeup->signal(state);
    //else if(fComplete)
	//(*fComplete)(fRefCon, state, fControl, this);
    return state;
}

bool IOFireWireAVCCommandInGen::init(IOFireWireNub *device, UInt32 generation,
            const UInt8 * command, UInt32 cmdLen, UInt8 * response, UInt32 * responseLen)
{
	FIRELOG_MSG(("IOFireWireAVCCommandInGen::init (this=0x%08X)\n",this));

    FWAddress addr;
    UInt32 dummyGen;
    IOFireWireController *control;
    // Get nodeID and check generation
    device->getNodeIDGeneration(dummyGen, fWriteNodeID);
    fWriteGen = generation;
    
    // setup quad write
    addr.nodeID		 = fWriteNodeID;
    addr.addressHi   = kCSRRegisterSpaceBaseAddressHi;
    addr.addressLo   = kFCPCommandAddress;

    control = device->getController();
    IOFWCommand::initWithController(control);
    // Start out without timeout, update when command is accepted by device
    fTimeout = 0;
    fSync = true;
    fCancelOnReset = true;
    fResponse = response;
    fResponseLen = responseLen;
    fMaxRetries = 4;
    fCurRetries = fMaxRetries;
    fCmdLen = cmdLen;
    
    // create command
    if(cmdLen == 4 || cmdLen == 8) {
        fMem = NULL;
        fCommand = command;
        fWriteCmd = new IOFWWriteQuadCommand;
        if(!fWriteCmd) {
            return false;
        }
        ((IOFWWriteQuadCommand *)fWriteCmd)->initAll(control, generation, addr,
                                            (UInt32 *)command, cmdLen/4, writeDone, this);
    }
    else {
        fCommand = NULL;
        fMem = IOMemoryDescriptor::withAddress((void *)command, cmdLen,
                    kIODirectionOutIn);
        if(!fMem) {
            return false;
        }
		
		IOReturn err = fMem->prepare();
		if( err != kIOReturnSuccess )
		{
			fMem->release();
			return false;
		}
		
        fWriteCmd = new IOFWWriteCommand;
        if(!fWriteCmd) {
            return false;
        }
        ((IOFWWriteCommand *)fWriteCmd)->initAll(control, generation, addr, fMem, writeDone, this);
    }
    return true;
}

IOReturn IOFireWireAVCCommandInGen::reinit(IOFireWireNub *device, UInt32 generation,
            const UInt8 * command, UInt32 cmdLen, UInt8 * response, UInt32 * responseLen)
{
	FIRELOG_MSG(("IOFireWireAVCCommandInGen::reinit (this=0x%08X)\n",this));

    if(Busy())
        return fStatus;
    if(fMem) {
        fMem->release();
        fMem = NULL;
    }
    if(fWriteCmd) {
        fWriteCmd->release();
        fWriteCmd = NULL;
    }
    if(init(device, generation, command, cmdLen, response, responseLen))
        return kIOReturnSuccess;
    else
        return kIOReturnNoMemory;
}
                                                    
