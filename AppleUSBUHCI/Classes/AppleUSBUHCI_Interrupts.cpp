/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.  
 * Please see the License for the specific language governing rights and 
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */



#include <libkern/OSByteOrder.h>
#include <IOKit/usb/IOUSBLog.h>
#include <IOKit/usb/IOUSBRootHubDevice.h>
#include "AppleUSBUHCI.h"
#include "AppleUHCIListElement.h"

#define super IOUSBControllerV2
#define self this

// ========================================================================
#pragma mark Interrupts
// ========================================================================

void 
AppleUSBUHCI::PollInterrupts(IOUSBCompletionAction safeAction)
{
    USBLog(1, "AppleUSBUHCI[%p]::PollInterrupts (unused)", this);
    // Not used
}



//Called at hardware interrupt time.
bool 
AppleUSBUHCI::PrimaryInterruptFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    register AppleUSBUHCI	*controller = (AppleUSBUHCI *)owner;
    bool					result = true;
	
    // If we our controller has gone away, or it's going away, or if we're on a PC Card and we have been ejected,
    // then don't process this interrupt.
    //
    // if (!controller || controller->isInactive() || (controller->_onCardBus && controller->_pcCardEjected) || !controller->_uhciAvailable)
    if (!controller || controller->isInactive() || !controller->_uhciAvailable)
        return false;

    // Process this interrupt
    //
    controller->_filterInterruptActive = true;
    result = controller->FilterInterrupt();
    controller->_filterInterruptActive = false;
    return result;
}



void 
AppleUSBUHCI::InterruptHandler(OSObject *owner, IOInterruptEventSource *source, int count)
{
    AppleUSBUHCI *controller = (AppleUSBUHCI *)owner;
    
    //USBLog(7, "AppleUSBUHCI::InterruptHandler");
	
    //
    // Interrupt source callouts are not blocked by _workLoop->sleep()
    // and driver checks _uhciBusState to prevent touching hardware when
    // UHCI has powered down, and another device sharing the interrupt
    // line has it asserted.
    //
    // Driver must ensure that the device cannot generate an interrupt
    // while in suspend state to prevent an interrupt storm.
    //
	
    if (controller && !controller->isInactive() && controller->_uhciAvailable) 
	{
        controller->HandleInterrupt();
    }
}



// ========= Interrupt handling ================


// Called at hardware interrupt time
bool
AppleUSBUHCI::FilterInterrupt(void)
{
	UInt16						activeInterrupts;
	AbsoluteTime				timeStamp;
    Boolean						needSignal = false;
	
	// we leave all interrupts enabled, so see which ones are active
	activeInterrupts = ioRead16(kUHCI_STS) & kUHCI_STS_INTR_MASK;
	
	if (activeInterrupts != 0) 
	{
		if (activeInterrupts & kUHCI_STS_HCPE)
		{
			// Host Controller Process Error - usually a bad data structure on the list
			_hostControllerProcessInterrupt = kUHCI_STS_HCPE;
			ioWrite16(kUHCI_STS, kUHCI_STS_HCPE);
			needSignal = true;
			//USBLog(1, "AppleUSBUHCI[%p]::FilterInterrupt - HCPE error - legacy reg = %p", this, (void*)_device->configRead16(kUHCI_PCI_LEGKEY));
		}
		if (activeInterrupts & kUHCI_STS_HSE)
		{
			// Host System Error - usually a PCI issue
			_hostSystemErrorInterrupt = kUHCI_STS_HSE;
			ioWrite16(kUHCI_STS, kUHCI_STS_HSE);
			needSignal = true;
			//USBLog(1, "AppleUSBUHCI[%p]::FilterInterrupt - HSE error - legacy reg = %p", this, (void*)_device->configRead16(kUHCI_PCI_LEGKEY));
		}
		if (activeInterrupts & kUHCI_STS_RD)
		{
			// Resume Detect - remote wakeup
			_resumeDetectInterrupt = kUHCI_STS_RD;
			ioWrite16(kUHCI_STS, kUHCI_STS_RD);
			needSignal = true;
		}
		if (activeInterrupts & kUHCI_STS_EI)
		{
			// USB Error Interrupt - transaction error (CRC, timeout, etc)
			_usbErrorInterrupt = kUHCI_STS_EI;
			ioWrite16(kUHCI_STS, kUHCI_STS_EI);
			needSignal = true;
		}
		if (activeInterrupts & kUHCI_STS_INT)
		{
			// Normal IOC interrupt - we need to check out low latency Isoch as well
            clock_get_uptime(&timeStamp);
			_usbCompletionInterrupt = kUHCI_STS_INT;
			ioWrite16(kUHCI_STS, kUHCI_STS_INT);
			needSignal = true;
			
			// we need to check the periodic list to see if there are any Isoch TDs which need to come off
			// and potentially have their frame lists updated (for Low Latency) we will place them in reverse
			// order on a "done queue" which will be looked at by the isoch scavanger
			// only do this if the periodic schedule is enabled
			if (!_inAbortIsochEP  && (_outSlot < kUHCI_NVFRAMES))
			{
				AppleUHCIIsochTransferDescriptor	*cachedHead;
				UInt32								cachedProducer;
				UInt16								curSlot, testSlot, nextSlot;
				
				curSlot = (ReadFrameNumber() & kUHCI_NVFRAMES_MASK);
				
				cachedHead = (AppleUHCIIsochTransferDescriptor*)_savedDoneQueueHead;
				cachedProducer = _producerCount;
				testSlot = _outSlot;
				
				while (testSlot != curSlot)
				{
					IOUSBControllerListElement				*thing, *nextThing;
					AppleUHCIIsochTransferDescriptor		*isochTD;
					
					nextSlot = (testSlot+1) & kUHCI_NVFRAMES_MASK;
					thing = _logicalFrameList[testSlot];
					while (thing != NULL)
					{
						nextThing = thing->_logicalNext;
						isochTD = OSDynamicCast(AppleUHCIIsochTransferDescriptor, thing);
						
						if (!isochTD)
							break;						// only care about Isoch in this list - if we get here we are at the interrupt TDs
												
						// need to unlink this TD
						_logicalFrameList[testSlot] = nextThing;
						_frameList[testSlot] = HostToUSBLong(thing->GetPhysicalLink());
						
						if (isochTD->_lowLatency)
							isochTD->frStatus = isochTD->UpdateFrameList(timeStamp);
						// place this guy on the backward done queue
						// the reason that we do not use the _logicalNext link is that the done queue is not a null terminated list
						// and the element linked "last" in the list might not be a true link - trust me
						isochTD->_doneQueueLink = cachedHead;
						cachedHead = isochTD;
						cachedProducer++;
						if (isochTD->_pEndpoint)
						{
							isochTD->_pEndpoint->onProducerQ++;
							isochTD->_pEndpoint->scheduledTDs--;
						}
						
						thing = nextThing;
					}
					testSlot = nextSlot;
					_outSlot = testSlot;
				}
				IOSimpleLockLock( _wdhLock );
				
				_savedDoneQueueHead = cachedHead;	// updates the shadow head
				_producerCount = cachedProducer;	// Validates _producerCount;
				
				IOSimpleLockUnlock( _wdhLock );
			}
		}
	}
	
	// We will return false from this filter routine,
	// but will indicate that there the action routine should be called
	// by calling _filterInterruptSource->signalInterrupt(). 
	// This is needed because IOKit will disable interrupts for a level interrupt
	// after the filter interrupt is run, until the action interrupt is called.
	// We want to be able to have our filter interrupt routine called
	// before the action routine runs, if needed.  That is what will enable
	// low latency isoch transfers to work, as when the
	// system is under heavy load, the action routine can be delayed for tens of ms.
	//
	if (needSignal)
		_interruptSource->signalInterrupt();
	
	return false;
}



// Called at software interrupt time
void
AppleUSBUHCI::HandleInterrupt(void)
{
	UInt16					status;
	UInt32					intrStatus;
	bool					needReset = false;
	UHCIAlignmentBuffer		*bp;
		
	status = ioRead16(kUHCI_STS);

	if (_hostControllerProcessInterrupt & kUHCI_STS_HCPE)
	{
		_hostControllerProcessInterrupt = 0;
		USBLog(1, "AppleUSBUHCI[%p]::HandleInterrupt - Host controller process error", this);
		needReset = true;
	}
	if (_hostSystemErrorInterrupt & kUHCI_STS_HSE)
	{
		_hostSystemErrorInterrupt = 0;
		USBLog(1, "AppleUSBUHCI[%p]::HandleInterrupt - Host controller system error(CMD:%p STS:%p INTR:%p PORTSC1:%p PORTSC2:%p FRBASEADDR:%p ConfigCMD:%p)", this,(void*)ioRead16(kUHCI_CMD), (void*)ioRead16(kUHCI_STS), (void*)ioRead16(kUHCI_INTR), (void*)ioRead16(kUHCI_PORTSC1), (void*)ioRead16(kUHCI_PORTSC2), (void*)ioRead32(kUHCI_FRBASEADDR), (void*)_device->configRead16(kIOPCIConfigCommand));
		needReset = true;
	}
	if (_resumeDetectInterrupt & kUHCI_STS_RD) 
	{
		_resumeDetectInterrupt = 0;
		USBLog(2, "AppleUSBUHCI[%p]::HandleInterrupt - Host controller resume detected", this);
		if (_uhciBusState == kUHCIBusStateSuspended) 
		{
			ResumeController();
		}
	}
	if (_usbErrorInterrupt & kUHCI_STS_EI) 
	{
		_usbErrorInterrupt = 0;
		USBLog(6, "AppleUSBUHCI[%p]::HandleInterrupt - Host controller error interrupt", this);
	}
	if (_usbCompletionInterrupt & kUHCI_STS_INT)
	{
		_usbCompletionInterrupt = 0;
		USBLog(7, "AppleUSBUHCI[%p]::HandleInterrupt - Normal interrupt", this);
		if (_consumerCount != _producerCount)
		{	
			USBLog(7, "AppleUSBUHCI[%p]::HandleInterrupt - Isoch was handled", this);
		}
	}
	
	if (needReset) 
	{
		IOSleep(1000);
		USBLog(1, "AppleUSBUHCI[%p]::HandleInterrupt - Resetting controller due to errors detected at interrupt time (0x%x)", this, status);
		Reset(true);
		Run(true);
	}

	ProcessCompletedTransactions();
	
	// Check for root hub status change
	RHCheckStatus();
}



