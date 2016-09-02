/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1998-2006 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <libkern/OSByteOrder.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/platform/ApplePlatformExpert.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/IOTimerEventSource.h>

#include <IOKit/usb/IOUSBRootHubDevice.h>
#include <IOKit/usb/IOUSBLog.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/libkern.h>

#ifndef kACPIDevicePathKey
#define kACPIDevicePathKey			"acpi-path"
#endif

#include "AppleUSBEHCI.h"

// From the file Gossamer.h that is not available
enum {
    kGossamerTypeGossamer = 1,
    kGossamerTypeSilk,
    kGossamerTypeWallstreet,
    kGossamerTypeiMac,
    kGossamerTypeYosemite,
    kGossamerType101
};

#include "AppleUSBEHCI.h"

#define super IOUSBControllerV2

// USB bus has two power states, off and on
#define number_of_power_states 2

// Note: This defines two states. off and on. In the off state, the bus is suspended. We
// really should have three state, off (reset), suspended (suspend), and on (operational)
static IOPMPowerState ourPowerStates[number_of_power_states] = {
	{1,0,0,0,0,0,0,0,0,0,0,0},
	{1,IOPMDeviceUsable,IOPMPowerOn,IOPMPowerOn,0,0,0,0,0,0,0,0}
};

static IOPMPowerState ourPowerStatesKL[number_of_power_states] = {
	{1,0,0,0,0,0,0,0,0,0,0,0},
  {1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn | IOPMClockNormal, 0,0,0,0,0,0,0,0}
};

extern UInt32 getPortSCForWriting(EHCIRegistersPtr _pEHCIRegisters, short port);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initForPM
//
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void 
AppleUSBEHCI::initForPM (IOPCIDevice *provider)
{
    //   We need to determine which EHCI controllers don't survive sleep.  These fall into 2 categories:
    //
    //   1.  CardBus cards
    //	 2.  PCI Cards that lose power (right now because of a bug in the PCI Family, USB PCI cards do not prevent
    //	     sleep, so even cards that don't support the PCI Power Mgmt stuff get their power removed.
    //
    //  So here, we look at all those cases and set the _unloadUIMAcrossSleep boolean to true.  As it turns out,
    //  if a controller does not have the "AAPL,clock-id" property, then it means that it cannot survive sleep.  We
    //  might need to refine this later once we figure how to deal with PCI cards that can go into PCI sleep mode.
    //  An exception is the B&W G3, that does not have this property but can sleep.  Sigh...
	
    //  Deal with CardBus USB cards.  Their provider will be a "IOCardBusDevice", as opposed to a "IOPCIDevice"
    //
    _onCardBus = (0 != provider->metaCast("IOCardBusDevice"));
    //  Now, look at PCI cards.  Note that the onboard controller's provider is an IOPCIDevice so we cannot use that
    //  to distinguish between USB PCI cards and the on board controller.  Instead, we use the existence of the
    //  "AAPL,clock-id" property in the provider.  If it does not exist, then we are a EHCI controller on a USB PCI card.
    //
    if ( !provider->getProperty("AAPL,clock-id") && !((getPlatform()->getChipSetType() == kChipSetTypeGossamer) && getPlatform()->getMachineType() == kGossamerTypeYosemite) )
    {
		bool			hasSupport = false;

		// ICH6 is only in the Developer Transition machines, and we will assume that it can support D3Cold
		if (_errataBits & kErrataICH6PowerSequencing)
			hasSupport = provider->hasPCIPowerManagement(kPCIPMCPMESupportFromD3Cold);
		else
			hasSupport = provider->hasPCIPowerManagement();
		
		if (hasSupport)
		{
			if (_errataBits & kErrataICH6PowerSequencing)
				hasSupport = (provider->enablePCIPowerManagement(kPCIPMCPMESupportFromD3Cold) == kIOReturnSuccess);
			else
				hasSupport = (provider->enablePCIPowerManagement() == kIOReturnSuccess);
		}
		if (hasSupport)
		{
			_hasPCIPwrMgmt = true;
            setProperty("Card Type","Built-in");
		}
        else
        {
            USBLog(1, "AppleUSBEHCI[%p]::start EHCI controller will be unloaded across sleep",this);
            _unloadUIMAcrossSleep = true;
            setProperty("Card Type","PCI");
        }
    }
    else
    {
        setProperty("Card Type","Built-in");
    }
    
    if ( _onCardBus )
    {
        setProperty("Card Type","CardBus");
        _unloadUIMAcrossSleep = true;
    }
    
    // callPlatformFunction symbols
    usb_remote_wakeup = OSSymbol::withCString("usb_remote_wakeup");
    registerService();  		//needed to find ::callPlatformFunction and then to wake Yosemite
	
    // register ourselves with superclass policy-maker
    if ( provider->getProperty("AAPL,clock-id")) 
    {
		USBLog(2, "AppleUSBEHCI[%p]:: registering controlling driver with clock",  this);
        registerPowerDriver(this,ourPowerStatesKL,number_of_power_states);
    }
    else 
    {
		USBLog(2, "AppleUSBEHCI[%p]:: registering controlling driver without clock",  this);
        registerPowerDriver(this,ourPowerStates,number_of_power_states);
    }
    changePowerStateTo(1);
    
	// this is for restarts and shut downs
	if (_errataBits & kErrataICH6PowerSequencing)
		_powerDownNotifier = registerPrioritySleepWakeInterest(PowerDownHandler, this, 0);
		
	// if we have an ExpressCard attached (non-zero port), then we will need to disable port resume 
	// for that port (some cards disconnect when the ExpressCard power goes away and we would like to ignore these extra detach events.
	_ExpressCardPort = ExpressCardPort(provider);	
	_badExpressCardAttached = false;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// maxCapabilityForDomainState
//
// Overrides superclass implementation, because kIOPMDoze is not in
// the power state array.
// Return that we can be in the On state if the system is On or in Doze.
// Otherwise return that we will be Off.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
unsigned long AppleUSBEHCI::maxCapabilityForDomainState ( IOPMPowerFlags domainState )
{
    if ( getProvider()->getProperty("AAPL,clock-id")) {
        if ( ((domainState & IOPMPowerOn) && (domainState & IOPMClockNormal) ) ||
			 (domainState & kIOPMDoze) && (domainState & IOPMClockNormal) ) {
            return 1;
        }
        else {
            return 0;
        }
    }
    else {					// non-keylargo system
        if ( (domainState & IOPMPowerOn) ||
			 (domainState & kIOPMDoze) ) {
            return 1;
        }
        else {
            return 0;
        }
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initialPowerStateForDomainState
//
// Overrides superclass implementation, because the EHCI has multiple
// parents that start up at different times.
// Return that we are in the On state at startup time.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
unsigned long AppleUSBEHCI::initialPowerStateForDomainState ( IOPMPowerFlags domainState )
{
    return 1;
}


//=============================================================================================
//
//	setPowerState
//
//	Called by the superclass to turn the controller on and off.  There are actually 3 different
// 	states: 
//		0 = suspended
//		1 = running
//		2 = idle suspend (suspended if nothing connected to the root hub but system is running.
//
//	The system will call us to go into state 0 or state 1.  We have an timer that looks for root hub
//	inactivity and when it sees such inactivity, it will call us with a level of 3.  When we then
//	detect a "resume" interrupt, we call setPowerState with a level of 1, running.
//
//=============================================================================================
//
IOReturn 
AppleUSBEHCI::setPowerState( unsigned long powerStateOrdinal, IOService* whatDevice )
{
    IOReturn				sleepRes;
    static uint32_t *		pHibernateState;
    
    USBLog(5,"AppleUSBEHCI[%p]::setPowerState (%ld) bus %ld",  this, powerStateOrdinal, _busNumber );
    IOSleep(5);
	
    //	If we are not going to sleep, then we need to take the gate, otherwise, we need to wake up    
    //
    if (_ehciBusState != kEHCIBusStateSuspended)
    {
        _workLoop->CloseGate();
    }
    else
    {
        sleepRes = _workLoop->wake(&_ehciBusState);
        if(sleepRes != kIOReturnSuccess) 
        {
            USBError(1, "AppleUSBEHCI[%p]::setPowerState - Can't wake  workloop, error 0x%x",  this, sleepRes);
        }
        else
        {
            USBLog(5, "AppleUSBEHCI[%p]::setPowerState - workLoop successfully awakened",  this);
        }
    }
    
    if ( powerStateOrdinal == kEHCISetPowerLevelSuspend ) 
    {
		// disable the interrupt
		_companionWakeHoldoff = true;						// from this point on, don't allow companion controllers to wake up until we have
		
		USBLog(5, "AppleUSBEHCI::setPowerState - disabling interrupt before suspending bus");
		_savedUSBIntr = _pEHCIRegisters->USBIntr;			// save currently enabled interrupts
		_pEHCIRegisters->USBIntr = 0;						// disable all interrupts

		if ( !pHibernateState )
		{
			OSData * data = OSDynamicCast(OSData, (IOService::getPMRootDomain())->getProperty(kIOHibernateStateKey));
			if (data)
			{
				pHibernateState = (uint32_t *) data->getBytesNoCopy();
			}
			
		}
		
        if ( _unloadUIMAcrossSleep )
        {
            USBLog(3,"AppleUSBEHCI[%p]::setPowerState - Unloading UIM for bus %d before going to sleep", this, (int)_busNumber );
            
            if ( _rootHubDevice )
            {
                USBLog(2, "AppleUSBEHCI[%p]::setPowerState - Terminating root hub in setPowerState()",  this);
                _rootHubDevice->terminate(kIOServiceRequired | kIOServiceSynchronous);
                _rootHubDevice->detachAll(gIOUSBPlane);
                _rootHubDevice->release();
                _rootHubDevice = NULL;
                USBLog(2, "AppleUSBEHCI[%p]::setPowerState - Terminated root hub in setPowerState()",  this);
            }
            SuspendUSBBus();
            UIMFinalizeForPowerDown();
			
            _ehciAvailable = false;					// tell the interrupt filter routine that we are off
        }
        else 
        {
            USBLog(5, "AppleUSBEHCI[%p]::setPowerState - suspending the bus",  this);
            _remote_wakeup_occurred = false;
			
            // Make sure that we have the USB Bus running before we go into suspend
            if (_idleSuspend)
            {
                USBLog(5, "AppleUSBEHCI[%p]::setPowerState - in _idleSuspend -  restarting USB before suspending", this);
                RestartUSBBus();
            }
			USBLog(7, "AppleUSBEHCI[%p]::setPowerState - about to suspend bus - showing queue", this);
			printAsyncQueue(7);
            SuspendUSBBus();

            USBLog(7, "AppleUSBEHCI[%p]::setPowerState The bus is now suspended - showing queue", this);
			printAsyncQueue(7);
        }
        
		_ehciAvailable = false;								// tell the interrupt filter routine that we are off
		
        _ehciBusState = kEHCIBusStateSuspended;
        _idleSuspend = false;
	}
    
    if ( powerStateOrdinal == kEHCISetPowerLevelIdleSuspend )
    {
        USBLog(5, "AppleUSBEHCI[%p]::setPowerState - halting the bus due to inactivity",  this);
        _idleSuspend = true;
        DisableAsyncSchedule(true);
        DisablePeriodicSchedule(true);
        StopUSBBus();
		
        USBLog(5, "AppleUSBEHCI[%p]::setPowerState - The bus is now halted due to inactivity",  this);
		
    }
    
    if ( powerStateOrdinal == kEHCISetPowerLevelRunning ) 
    {
		
		// If we woke from hibernate, unload the UIM and suspend the bus
		//
		if ( _uimInitialized && pHibernateState && *pHibernateState && !_wakingFromHibernation && !_idleSuspend)
		{
			USBLog(1,"AppleUSBEHCI[%p]::setPowerState - Unloading UIM for bus %d after hibernate - _ehciAvailable[%s]",this, (int)_busNumber, _ehciAvailable ? "true" : "false" );
			_wakingFromHibernation = true;						// we will clear this when we create the root hub
			UIMFinalizeForPowerDown();
			_ehciAvailable = true;								// let the delete endpoints come through
			if ( _rootHubDevice )
			{
				USBLog(2, "+AppleUSBEHCI[%p]::setPowerState - Terminating root hub in setPowerState()",  this);
				_rootHubDevice->terminate(kIOServiceRequired | kIOServiceSynchronous);
				_rootHubDevice->detachAll(gIOUSBPlane);
				_rootHubDevice->release();
				_rootHubDevice = NULL;
				USBLog(2, "-AppleUSBEHCI[%p]::setPowerState - Terminated root hub in setPowerState()",  this);
			}
			// SuspendUSBBus();
			// _ehciAvailable = false;					// tell the interrupt filter routine that we are off
			
			IOSleep(50);								// this appears to the devices as a reset, so wait the required 50 ms
			USBLog(2,"AppleUSBEHCI[%p]::setPowerState - spawning root hub creation thread", this);
			thread_call_enter(_rootHubCreationThread);
			goto done;
			// we will clear _companionWakeHoldoff when we are done creating the EHCI root hub
		}
		
        // If we were just idle suspended, we did not unload the UIM, so we need to check that here
        //
        else if ( !_uimInitialized )
        {
            // If we are inactive OR if we are a PC Card and we have been ejected, then we don't need to do anything here
            //
            if ( isInactive() || (_onCardBus && _pcCardEjected) )
            {
                _ehciBusState = kEHCIBusStateRunning;
                USBLog(3,"AppleUSBEHCI[%p]::setPowerState - isInactive (or pccardEjected) while setPowerState (%d,%d)",this, isInactive(), _pcCardEjected);
            }
            else
            {
                IOReturn	err = kIOReturnSuccess;
				
                USBLog(5, "AppleUSBEHCI[%p]::setPowerState - Re-loading UIM if necessary (%d)",  this, _uimInitialized );
				
                // Initialize our hardware
                //
                UIMInitializeForPowerUp();
				
				_ehciBusState = kEHCIBusStateRunning;
                _ehciAvailable = true;										// tell the interrupt filter routine that we are on
                
				// At this point we would create the RootHub device that loads all drivers.  However, we need to wait until all other
				// controllers are terminated.
				
				if ( _rootHubDevice == NULL )
                {
                    err = CreateRootHubDevice( _device, &_rootHubDevice );
                    if ( err != kIOReturnSuccess )
                    {
                        USBError(1,"AppleUSBEHCI[%p]::setPowerState - Could not create root hub device upon wakeup (%x)!", this, err);
                    }
                    else
                    {
                        _rootHubDevice->registerService(kIOServiceRequired | kIOServiceSynchronous);
                    }
                }
            }
        }
		else if (_idleSuspend)
		{
            USBLog(5, "AppleUSBEHCI[%p]::setPowerState - in _idleSuspend -  restarting USB",  this);
			RestartUSBBus();
		}
        else 
        {
            USBLog(5, "AppleUSBEHCI[%p]::setPowerState - setPowerState powering on USB",  this);
			
			// at this point, interrupts are disabled, and we are waking up. If the Port Change interrupt is active
			// then it is likely that we are responsible for the system issuing the wakeup
			if (USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCIPortChangeIntBit)
			{
				IOLog("USB caused wake event (EHCI)\n");
			}
			
            _remote_wakeup_occurred = true;	//doesn't matter how we woke up
			
			_ehciAvailable = true;										// tell the interrupt filter routine that we are on

			if (_savedUSBIntr)
				_pEHCIRegisters->USBIntr = _savedUSBIntr;							// enable all interrupts
			USBLog(5, "AppleUSBEHCI[%p]::setPowerState - after reenabling interrupts, USBIntr = %p", this, (void*)USBToHostLong(_pEHCIRegisters->USBIntr));

			USBLog(7, "AppleUSBEHCI[%p]::setPowerState - about to resume bus - showing queue", this);
			printAsyncQueue(7);
            ResumeUSBBus();
			USBLog(7, "AppleUSBEHCI[%p]::setPowerState - bus has been resumed - showing queue", this);
			printAsyncQueue(7);
            _ehciBusState = kEHCIBusStateRunning;
        }
				
		_companionWakeHoldoff = false;
        LastRootHubPortStatusChanged(true);
        _idleSuspend = false;
    }
	
done:	
    // if we are now suspended, then we need to sleep our workloop, otherwise, we need to release the gate on it
    //
    if (_ehciBusState == kEHCIBusStateSuspended)
    {
        sleepRes = _workLoop->sleep(&_ehciBusState);
        if(sleepRes != kIOReturnSuccess) 
        {
            USBError(1, "AppleUSBEHCI[%p]::setPowerState - Can't sleep workloop, error 0x%x",  this, sleepRes);
        }
        else
		{
            USBLog(5, "AppleUSBEHCI[%p]::setPowerState - workLoop successfully slept",  this);
        }
    }
    else
    {
        _workLoop->OpenGate();
    }
	
    USBLog(5,"AppleUSBEHCI[%p]::setPowerState done",  this );
    return IOPMAckImplied;
}


IOReturn AppleUSBEHCI::callPlatformFunction(const OSSymbol *functionName,
											bool waitForFunction,
											void *param1, void *param2,
											void *param3, void *param4)
{  
    USBLog(3, "%s[%p]::callPlatformFunction(%s)", getName(), this, functionName->getCStringNoCopy());

    if (functionName == usb_remote_wakeup)
    {
		bool	*wake;
		
		wake = (bool *)param1;
		
		if (_remote_wakeup_occurred)
		{
			*wake = true;
		}
		else
		{
			*wake = false;
		}
    	return kIOReturnSuccess;
    }
	
    return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}


void			
AppleUSBEHCI::ResumeUSBBus()
{
    UInt8	numPorts;
	int		i;
    bool	enabledports = false;

    // restore volatile registers saved in SuspendUSBBus()
	if (USBToHostLong(_pEHCIRegisters->ConfigFlag) != kEHCIPortRoutingBit)
	{
		USBError(1, "USB EHCI[%p] - Configure Flag Register appears to have been lost - power issue?", this);
		
		USBLog(5, "AppleUSBEHCI[%p]::ResumeUSBBus - restoring ConfigFlag[from 0x%x]",  this, (unsigned int) USBToHostLong(_pEHCIRegisters->ConfigFlag));
		_pEHCIRegisters->ConfigFlag = HostToUSBLong(kEHCIPortRoutingBit);
		IOSync();
		if (_errataBits & kErrataNECIncompleteWrite)
		{
			UInt32		newValue = 0, count = 0;
			newValue = USBToHostLong(_pEHCIRegisters->ConfigFlag);
			while ((count++ < 10) && (newValue != kEHCIPortRoutingBit))
			{
				USBError(1, "EHCI driver: ResumeUSBBus - ConfigFlag bit not sticking. Retrying.");
				_pEHCIRegisters->ConfigFlag = HostToUSBLong(kEHCIPortRoutingBit);
				IOSync();
				newValue = USBToHostLong(_pEHCIRegisters->ConfigFlag);
			}
		}
	}
	
    if (_savedPeriodicListBase && (_pEHCIRegisters->PeriodicListBase != _savedPeriodicListBase))
    {
		USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - restoring PeriodicListBase[from 0x%x to 0x%x]",  this, (unsigned int)USBToHostLong(_pEHCIRegisters->PeriodicListBase),  (unsigned int)USBToHostLong(_savedPeriodicListBase));
        _pEHCIRegisters->PeriodicListBase = _savedPeriodicListBase;
        IOSync();
    }
	
    if (_savedAsyncListAddr && (_pEHCIRegisters->AsyncListAddr != _savedAsyncListAddr))
    {
		USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - restoring AsyncListAddr[from 0x%x to 0x%x]",  this,  (unsigned int)USBToHostLong(_pEHCIRegisters->AsyncListAddr),  (unsigned int)USBToHostLong(_savedAsyncListAddr));
        _pEHCIRegisters->AsyncListAddr = _savedAsyncListAddr;
        IOSync();
    }

	if (_is64bit)	
	    _pEHCIRegisters->CTRLDSSegment = 0;

	// 09-15-2005 rdar://4041217
	// make sure that the HC is actually turned on (as long as it was before we suspended)
	// without this, we were resuming the ports and taking them out of resume and not sending SOF tokens
	// which was confusing lots of devices.
	if (USBToHostLong(_savedUSBCMD) & kEHCICMDRunStop)
	{
		// if the controller was running before, go ahead and turn it on now, but leave the schedules off
		_pEHCIRegisters->USBCMD |= HostToUSBLong(kEHCICMDRunStop);
		IOSync();
		for (i=0; (i< 100) && (USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCIHCHaltedBit); i++)
			IODelay(100);
		if (i>1)
		{
			USBError(1, "AppleUSBEHCI[%p]::ResumeUSBBus - controller took (%d) turns to get going", this, i);
		}
	}
    // resume all enabled ports which we own
    numPorts = USBToHostLong(_pEHCICapRegisters->HCSParams) & kEHCINumPortsMask;
    USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - resuming %d ports",  this, numPorts);
    for (i=0; i < numPorts; i++)
    {
		UInt32 portStat;
		portStat = getPortSCForWriting(_pEHCIRegisters, i+1);
		if (portStat & kEHCIPortSC_Owner)
		{
			USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d owned by OHCI",  this, i+1);
		}
		else if (portStat & kEHCIPortSC_Enabled)
		{
			// is this an ExpressCard port that we disabled resume enable?  - if so, put it back!
			if (_badExpressCardAttached && ((int)_ExpressCardPort == (i+1))){
				portStat |= (kEHCIPortSC_WKCNNT_E|kEHCIPortSC_WKDSCNNT_E);
				_pEHCIRegisters->PortSC[i] = USBToHostLong(portStat);
				IOSync();
			}
			// If the port was NOT suspended prior to the suspend OR it is the cause of the resume, then
			// resume the port
			if ( !(_savedSuspendedPortBitmap & (1<<i)) || (portStat & kEHCIPortSC_Resume) )
			{
				portStat |= kEHCIPortSC_Resume;
				_pEHCIRegisters->PortSC[i] = HostToUSBLong(portStat);
				IOSync();
				if (_errataBits & kErrataNECIncompleteWrite)
				{
					UInt32		newValue = 0, count = 0;
					newValue = USBToHostLong(_pEHCIRegisters->PortSC[i]);
					while ((count++ < 10) && !(newValue & kEHCIPortSC_Resume))
					{
						USBError(1, "EHCI driver: ResumeUSBBus - PortSC resume not sticking (on). Retrying.");
						_pEHCIRegisters->PortSC[i] = HostToUSBLong(portStat);
						IOSync();
						newValue = USBToHostLong(_pEHCIRegisters->PortSC[i]);
					}
				}

				if ( (portStat & kEHCIPortSC_Resume) && (_savedSuspendedPortBitmap & (1<<i)))
				{
					USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d was suspended but resuming it because it generated a resume",  this, i+1);
				}

				USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d now resumed (%ld, %ld)",  this, i+1, _savedSuspendedPortBitmap & (1<<i), portStat & kEHCIPortSC_Resume);
			}
			else
				USBLog(4, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d not resuming because it was previously suspended",  this, i+1);

			enabledports = true;
		}
		else
		{
			USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d not enabled",  this, i);
		}
    }
    
    if (enabledports)
    {
		USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus Delaying 20 milliseconds in resume state",  this);
		IODelay(20000);
		for (i=0; i < numPorts; i++)
		{
			UInt32 portStat;
			portStat = getPortSCForWriting(_pEHCIRegisters, i+1);
			if (portStat & kEHCIPortSC_Owner)
			{
				USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d owned by OHCI",  this, i+1);
			}
			else if (portStat & kEHCIPortSC_Enabled)
			{
				portStat &= ~kEHCIPortSC_Resume;
				_pEHCIRegisters->PortSC[i] = HostToUSBLong(portStat);
				IOSync();
				
				// The following code, added because of rdar://4164872> NEC errata workarounds
				// caused <rdar://problem/4186241> Master Bug: M23 loses HS devices upon wake
				// We need to know why that is happenning, but for now, we will prevent its execution.
				// JRH - 09-15-2005 - with the fix for rdar://4041217 this should be able to be re-enabled
#if 0
				if (_errataBits & kErrataNECIncompleteWrite)
				{
					UInt32		newValue = 0, count = 0;
					IODelay(2000);										// The HC must transition the bit within 2ms
					newValue = USBToHostLong(_pEHCIRegisters->PortSC[i]);
					while ((count++ < 10) && (newValue & kEHCIPortSC_Resume))
					{
						USBError(1, "EHCI driver: ResumeUSBBus - PortSC resume not sticking (off). Retrying.");
						_pEHCIRegisters->PortSC[i] = HostToUSBLong(portStat);
						IOSync();
						IODelay(2000);										// The HC must transition the bit within 2ms
						newValue = USBToHostLong(_pEHCIRegisters->PortSC[i]);
					}
				}
#endif
				USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d finished resume sequence",  this, i+1);
				enabledports = true;
			}
			else
			{
				USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - port %d not enabled",  this, i+1);
			}
		}
		IODelay(10000);				// wait 10 ms before allowing any data transfer
    }
    
    if (_savedUSBCMD)
    {
		USBLog(7, "AppleUSBEHCI[%p]::ResumeUSBBus - USBCMD is <%p> will be <%p>",  this, (void*)_pEHCIRegisters->USBCMD, (void*)_savedUSBCMD);
		_pEHCIRegisters->USBCMD = _savedUSBCMD;
    }
	
}



void			
AppleUSBEHCI::SuspendUSBBus()
{
    UInt8	numPorts;
    int		i;
    UInt32	usbcmd, usbsts;
    
	// Initialize our suspended bitmap to no ports suspended
	_savedSuspendedPortBitmap = 0;
	
    // save the USBCMD register before disabling the list processing
    _savedUSBCMD = _pEHCIRegisters->USBCMD;
    USBLog(7, "AppleUSBEHCI[%p]::SuspendUSBBus - got _savedUSBCMD <%p>",  this, (void*)_savedUSBCMD);
    
	// disable list processing
    usbcmd = USBToHostLong(_savedUSBCMD);
	

	// first make sure that the status register matches the command register
    if (usbcmd & kEHCICMDAsyncEnable)
	{
		// if the status is currently off, we are waiting for it to come on from some previous event - this 
		// should really never happen
		for (i=0; (i < 100) && !(USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCISTSAsyncScheduleStatus); i++)
			IODelay(100);
		if (i)
		{
			USBError(1, "AppleUSBEHCI[%p]::SuspendUSBBus - Async Schedule should have been on but was off for %d loops", this, i);
		}
		usbcmd &= ~kEHCICMDAsyncEnable;
	}
    if (usbcmd & kEHCICMDPeriodicEnable)
	{
		// if the status is currently off, we are waiting for it to come on from some previous event - this 
		// should really never happen
		for (i=0; (i < 100) && !(USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCISTSPeriodicScheduleStatus); i++)
			IODelay(100);

		if (i)
		{
			USBError(1, "AppleUSBEHCI[%p]::SuspendUSBBus - Periodic Schedule should have been on but was off for %d loops", this, i);
		}
		usbcmd &= ~kEHCICMDPeriodicEnable;
	}
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);			// This will turn off both schedules if they aren't already off
    IOSync();
    
	// 09-15-2005 rdar://4041217
	// make sure that the lists are actually off before we start suspending the ports
	// otherwise, we end up trying to process a TD to a suspended (and blocked) port
	// which causes an incorrect error
	for (i=0; (i < 100) && (USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCISTSAsyncScheduleStatus); i++)
		IODelay(1000);
		
	if (i > 2)
	{
		USBError(1, "AppleUSBEHCI[%p]::SuspendUSBBus - Async Schedule took %d loops to turn off", this, i);
	}


	for (i=0; (i < 1000) && (USBToHostLong(_pEHCIRegisters->USBSTS) & kEHCISTSPeriodicScheduleStatus); i++)
		IODelay(1000);
		
	if (i > 2)
	{
		USBError(1, "AppleUSBEHCI[%p]::SuspendUSBBus - Periodic Schedule took %d loops to turn off CMD(%p) STS(%p)", this, i, (void*)USBToHostLong(_pEHCIRegisters->USBCMD), (void*)USBToHostLong(_pEHCIRegisters->USBSTS));
	}

	// save these registers per Intel recommendations - but do it AFTER the scheduling has stopped
    _savedPeriodicListBase = _pEHCIRegisters->PeriodicListBase;
    _savedAsyncListAddr = _pEHCIRegisters->AsyncListAddr;
	
    // suspend all enabled ports which we own
    GetNumberOfPorts( &numPorts );
    USBLog(numPorts ? 4 : 1, "AppleUSBEHCI[%p]::SuspendUSBBus - suspending %d ports",  this, numPorts);
    for (i=0; i < numPorts; i++)
    {
		UInt32 portStat;
		portStat = getPortSCForWriting(_pEHCIRegisters, i+1);
		if (portStat & kEHCIPortSC_Owner)
		{
			USBLog(4, "AppleUSBEHCI[%p]::SuspendUSBBus - port %d owned by OHCI",  this, i+1);
		}
		else if (portStat & kEHCIPortSC_Enabled)
		{
			// is this an ExpressCard port that needs to turn off resume enable?
			if (_badExpressCardAttached && ((int)_ExpressCardPort == (i+1))){
				portStat &= ~(kEHCIPortSC_WKCNNT_E|kEHCIPortSC_WKDSCNNT_E);
				_pEHCIRegisters->PortSC[i] = USBToHostLong(portStat);
				IOSync();
			}
			
			if (portStat & kEHCIPortSC_Suspend)
			{
				_savedSuspendedPortBitmap |= (1<<i);
				
				USBLog(4, "AppleUSBEHCI[%p]::SuspendUSBBus - port %d was already suspended",  this, i+1);
			}
			else
			{
				EHCIRootHubPortSuspend(i+1, true);
			}
		}
		else
		{
			USBLog(4, "AppleUSBEHCI[%p]::SuspendUSBBus - port %d not enabled",  this, i+1);
		}
    }

    // clear run/stop
    usbcmd &= ~kEHCICMDRunStop;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    IOSync();
    _ehciBusState = kEHCIBusStateOff;
    USBLog(7, "AppleUSBEHCI[%p]::SuspendUSBBus - ports suspended, HC stop set, waiting for halted",  this);
    
    // wait for halted bit
    do
    {
		usbsts = USBToHostLong(_pEHCIRegisters->USBSTS);
    } while (!(usbsts & kEHCIHCHaltedBit));
    
    USBLog(5, "AppleUSBEHCI[%p]::SuspendUSBBus - HC halted",  this);
}



void			
AppleUSBEHCI::StopUSBBus()
{
    UInt32	usbcmd;
    

    usbcmd = USBToHostLong(_pEHCIRegisters->USBCMD);
    // clear run/stop
    usbcmd &= ~kEHCICMDRunStop;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    _ehciBusState = kEHCIBusStateOff;
    USBLog(5, "AppleUSBEHCI[%p]::StopUSBBus - HC halted",  this);
}



void			
AppleUSBEHCI::RestartUSBBus()
{
    UInt32	usbcmd, usbsts;
    // wait for halted bit
    do
    {
		usbsts = USBToHostLong(_pEHCIRegisters->USBSTS);
    } while (!(usbsts & kEHCIHCHaltedBit));
    usbcmd = USBToHostLong(_pEHCIRegisters->USBCMD);
    // set run/stop
    usbcmd |= kEHCICMDRunStop;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    _ehciBusState = kEHCIBusStateRunning;
    USBLog(5, "AppleUSBEHCI[%p]::RestartUSBBus - HC restarted",  this);
}


IOReturn 
AppleUSBEHCI::PowerDownHandler(void *target, void *refCon, UInt32 messageType, IOService *service,
                                       void *messageArgument, vm_size_t argSize )
{
    AppleUSBEHCI *	me = OSDynamicCast(AppleUSBEHCI, (OSObject *)target);
    
    if (!me || !(me->_errataBits & kErrataICH6PowerSequencing))
        return kIOReturnUnsupported;
    
    USBLog(2, "AppleUSBEHCI[%p]::PowerDownHandler PowerDownHandler %p %p", me, (void*)messageType, messageArgument);

    if (me->_ehciAvailable) 
	{
        switch (messageType)
        {
            case kIOMessageSystemWillRestart:
            case kIOMessageSystemWillPowerOff:
				
                if (me->_ehciBusState == kEHCIBusStateRunning) 
				{
                    if ( me->_rootHubDevice ) 
					{
                        me->_rootHubDevice->terminate(kIOServiceRequired | kIOServiceSynchronous);
                        me->_rootHubDevice->detachAll(gIOUSBPlane);
                        me->_rootHubDevice->release();
                        me->_rootHubDevice = NULL;
                    }
                    me->SuspendUSBBus();
                }

				// Let's not look for any timeouts anymore
				// NOTE: This really should be done in the superclass, but there was no good way to do that in the time frame
				// we had. The PowerDownHandler should just be moved to the controller level
				me->_watchdogUSBTimer->cancelTimeout();
				
				if (me->_uimInitialized) 
				{
                    me->UIMFinalizeForPowerDown();
                }
                break;
                
            default:
                // We don't care about any other message that comes in here.
                break;
                
        }
    }
    return kIOReturnSuccess;
}

static IOACPIPlatformDevice * CopyACPIDevice( IORegistryEntry * device )
{
	IOACPIPlatformDevice *  acpiDevice = 0;
	OSString *				acpiPath;

	if (device)
	{
		acpiPath = (OSString *) device->copyProperty(kACPIDevicePathKey);
		if (acpiPath && !OSDynamicCast(OSString, acpiPath))
		{
			acpiPath->release();
			acpiPath = 0;
		}

		if (acpiPath)
		{
			IORegistryEntry * entry;

			entry = IORegistryEntry::fromPath(acpiPath->getCStringNoCopy());
			acpiPath->release();

			if (entry && entry->metaCast("IOACPIPlatformDevice"))
				acpiDevice = (IOACPIPlatformDevice *) entry;
			else if (entry)
				entry->release();
		}
	}

	return (acpiDevice);
}

static bool HasExpressCardUSB( IORegistryEntry * acpiDevice, UInt32 * portnum )
{
	const IORegistryPlane *	acpiPlane;
	bool					match = false;
	IORegistryIterator *	iter;
	IORegistryEntry *		entry;

	do {
		acpiPlane = acpiDevice->getPlane( "IOACPIPlane" );
		if (!acpiPlane)
			break;

		// acpiDevice is the USB controller in ACPI plane.
		// Recursively iterate over children of acpiDevice.

		iter = IORegistryIterator::iterateOver(
				/* start */	acpiDevice,
				/* plane */	acpiPlane,
				/* options */ kIORegistryIterateRecursively);

		if (iter)
		{
			while (!match && (entry = iter->getNextObject()))
			{
				// USB port must be a leaf node (no child), and
				// must be an IOACPIPlatformDevice.

				if ((entry->getChildEntry(acpiPlane) == 0) &&
					entry->metaCast("IOACPIPlatformDevice"))
				{
					IOACPIPlatformDevice * port;
					port = (IOACPIPlatformDevice *) entry;

					// Express card port? Is port ejectable?

					if (port->validateObject( "_EJD" ) == kIOReturnSuccess)
					{
						// Determining the USB port number.
						if (portnum)
							*portnum = strtoul(port->getLocation(), NULL, 10);
						match = true;
					}
				}
			}

			iter->release();
		}
	}
	while (false);
	
	return match;
}

// Checks for ExpressCard connected to this controller, and returns the port number (1 based)
// Will return 0 if no ExpressCard is connected to this controller.
//
UInt32 
AppleUSBEHCI::ExpressCardPort( IOService * provider )
{
	IOACPIPlatformDevice *	acpiDevice;
	UInt32					portNum = 0;
	bool					isPCIeUSB;
	
	acpiDevice = CopyACPIDevice( provider );
	if (acpiDevice)
	{
		isPCIeUSB = HasExpressCardUSB( acpiDevice, &portNum );	
		acpiDevice->release();
	}
	return(portNum);
}



void
AppleUSBEHCI::SynchronizeCompanionRootHub(IOUSBControllerV2* companion)
{
	int		i = 0;
	
	USBLog(4, "AppleUSBEHCI[%p]::SynchronizeCompanionRootHub for %s companion[%p]", this, companion->getName(), companion);
	if (!_companionWakeHoldoff)
	{
		USBLog(2, "AppleUSBEHCI[%p]::SynchronizeCompanionRootHub - no holdoff needed", this);
	}
	else
	{
		while (_companionWakeHoldoff)
		{
			if ((i++ % 100) == 0)
			{
				USBLog(2, "AppleUSBEHCI[%p]::SynchronizeCompanionRootHub - waiting on behalf of companion[%p]", this, companion);
			}
			IOSleep(10);		// wait for 10 milliseconds before checking again
			
			// After 10 seconds, give up
			if ( i > 1000)
			{
				USBLog(2, "AppleUSBEHCI[%p]::SynchronizeCompanionRootHub - Companion controller[%p] did not come up after 10 seconds!", this, companion);
				USBError(1, "USB:  The companion controller[%p] for %p did not come up after 10 seconds!", companion, this);
			}
		}
		USBLog(2, "AppleUSBEHCI[%p]::SynchronizeCompanionRootHub - companion[%p] is ready to go!!", this, companion);
	}
}
