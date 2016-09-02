/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1998-2007 Apple Inc.  All Rights Reserved.
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
#include <libkern/OSDebug.h>

#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>

#include <UserNotification/KUNCUserNotifications.h>

#include <IOKit/usb/IOUSBLog.h>

#include <IOKit/usb/IOUSBInterface.h>
#include <IOKit/usb/IOUSBRootHubDevice.h>
#include <IOKit/usb/IOUSBPipe.h>
#include <IOKit/usb/IOUSBControllerV2.h>
#include <IOKit/usb/IOUSBControllerV3.h>

#include "AppleUSBHub.h"
#include "AppleUSBHubPort.h"

#define super IOUSBHubPolicyMaker
#define self this

static ErrataListEntry	errataList[] = {

/* For the Cherry 4 port KB, From Cherry:
We use the bcd_releasenumber-highbyte for hardware- and the lowbyte for
firmwarestatus. We have 2 different for the hardware 03(eprom) and
06(masked microcontroller). Firmwarestatus is 05 today.
So high byte can be 03 or 06 ----  low byte can be 01, 02, 03, 04, 05

Currently we are working on a new mask with the new descriptors. The
firmwarestatus will be higher than 05.
*/
      {0x046a, 0x003, 0x0301, 0x0305, kErrataCaptiveOKBit}, // Cherry 4 port KB
      {0x046a, 0x003, 0x0601, 0x0605, kErrataCaptiveOKBit}  // Cherry 4 port KB
};

// from the EHCI driver
enum
{
    kEHCITestMode_Off		= 0,
    kEHCITestMode_J_State	= 1,
    kEHCITestMode_K_State 	= 2,
    kEHCITestMode_SE0_NAK	= 3,
    kEHCITestMode_Packet	= 4,
    kEHCITestMode_ForceEnable	= 5,
    kEHCITestMode_Start		= 10,
    kEHCITestMode_End		= 11
};

#define errataListLength (sizeof(errataList)/sizeof(ErrataListEntry))

OSDefineMetaClassAndStructors(AppleUSBHub, IOUSBHubPolicyMaker)

#define  WATCHDOGSECONDS	6
enum
{
	kWatchdogTimerPeriod	=	1000 * WATCHDOGSECONDS,			// Issue our watchdog timer every WATCHDOGSECONDS sec
	kDevZeroTimeoutCount	=   30 / WATCHDOGSECONDS,			// We will look at dev zero locks every # of these
	kHubDriverRetryCount	=	3,

};

#pragma mark �������� IOService Methods ���������
bool 
AppleUSBHub::init( OSDictionary * propTable )
{
    if( !super::init(propTable))
        return (false);

    _numCaptive = 0;
    _startupDelay = 0;
    _timerSource = NULL;
    _gate = NULL;
    _portSuspended = false;
    _hubHasBeenDisconnected = false;
    _hubIsDead = false;
    _workThread = NULL;
    _resetPortZeroThread = NULL;
    _hubDeadCheckThread =  NULL;
    _busPowerGood = false;
    _powerForCaptive = 0;
    _outstandingIO = 0;
    _outstandingResumes = 0;
	_raisedPowerStateCount = 0;
    _needToClose = false;
	_abortExpected = false;
	_needInterruptRead = false;
	_devZeroLockedTimeoutCounter = kDevZeroTimeoutCount;
	_retryCount = kHubDriverRetryCount;
	_checkPortsThreadActive	= false;
	_doPortActionLock = IOLockAlloc();
    if (!_doPortActionLock)
	{
        return false;
	}
	
	return(true);
}



bool 
AppleUSBHub::start(IOService * provider)
{
	USBLog(7, "AppleUSBHub[%p]::start - This is handled by the superclass", this);
	return super::start(provider);
}


bool
AppleUSBHub::ConfigureHubDriver(void)
{
	// this is called as part of AppleUSBHub::start
    IOReturn				err = 0;
    OSDictionary			*providerDict;
    OSNumber				*errataProperty;
    OSNumber				*locationIDProperty;
	
    _inStartMethod = true;
	USBLog(5, "AppleUSBHub[%p]::ConfigureHubDriver - calling RaisePowerState and IncrementOutstandingIO", this);
	RaisePowerState();
    IncrementOutstandingIO();								// make sure we don't close until start is done
	
   // Create the timeout event source
    //
    _timerSource = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) TimeoutOccurred);
	
    if ( _timerSource == NULL )
    {
        USBError(1, "AppleUSBHub::start Couldn't allocate timer event source");
        goto ErrorExit;
    }
	
    
    _gate = IOCommandGate::commandGate(this);
	
    if(!_gate)
    {
		USBError(1, "AppleUSBHub[%p]::start - unable to create command gate", this);
        goto ErrorExit;
    }
	
	_workLoop = getWorkLoop();
    if ( !_workLoop )
    {
        USBError(1, "AppleUSBHub::start Couldn't get provider's workloop");
        goto ErrorExit;
    }
	
    // Keep a reference to our workloop
    //
    _workLoop->retain();
	
    if ( _workLoop->addEventSource( _timerSource ) != kIOReturnSuccess )
    {
        USBError(1, "AppleUSBHub::start Couldn't add timer event source");
        goto ErrorExit;
    }
	
    if ( _workLoop->addEventSource( _gate ) != kIOReturnSuccess )
    {
        USBError(1, "AppleUSBHub::start Couldn't add gate event source");
        goto ErrorExit;
    }
    
    _address	= _device->GetAddress();
	
    if (!_device->open(this))
    {
        USBError(1, "AppleUSBHub::start unable to open provider");
        goto ErrorExit;
    }
	
    // Check to see if we have an errata for the startup delay and if so,
    // then sleep for that amount of time.
    //
    errataProperty = (OSNumber *)_device->getProperty("kStartupDelay");
    if ( errataProperty )
    {
        _startupDelay = errataProperty->unsigned32BitValue();
        IOSleep( _startupDelay );
    }
    
    // Get the other errata that are not personality based
    //
    _errataBits = GetHubErrataBits();
    
    // Go ahead and configure the hub
    //
    err = ConfigureHub();
    
    if ( err == kIOReturnSuccess )
    {		
		if (_hsHub)
			registerService();					// for the benefit of a user client
		        
        // allocate a thread_call structure
        _workThread = thread_call_allocate((thread_call_func_t)ProcessStatusChangedEntry, (thread_call_param_t)this);
        _resetPortZeroThread = thread_call_allocate((thread_call_func_t)ResetPortZeroEntry, (thread_call_param_t)this);
        _hubDeadCheckThread = thread_call_allocate((thread_call_func_t)CheckForDeadHubEntry, (thread_call_param_t)this);
        _clearFeatureEndpointHaltThread = thread_call_allocate((thread_call_func_t)ClearFeatureEndpointHaltEntry, (thread_call_param_t)this);
        _checkForActivePortsThread = thread_call_allocate((thread_call_func_t)CheckForActivePortsEntry, (thread_call_param_t)this);
        _waitForPortResumesThread = thread_call_allocate((thread_call_func_t)WaitForPortResumesEntry, (thread_call_param_t)this);
        
        if ( !_workThread || !_resetPortZeroThread || !_hubDeadCheckThread || !_clearFeatureEndpointHaltThread || !_checkForActivePortsThread || !_waitForPortResumesThread)
        {
            USBError(1, "AppleUSBHub[%p] could not allocate all thread functions.  Aborting start", this);
            goto ErrorExit;
        }
		
        locationIDProperty = (OSNumber *) _device->getProperty(kUSBDevicePropertyLocationID);
        if ( locationIDProperty )
        {
            _locationID = locationIDProperty->unsigned32BitValue();
        }
        USBLog(1, "AppleUSBHub[%p]::start -  USB Generic Hub @ %d (0x%lx)", this, _address, _locationID);
 		_inStartMethod = false;
		
		USBLog(5, "AppleUSBHub[%p]::start - device name %s - done", this, _device->getName());
        DecrementOutstandingIO();
		LowerPowerState();
		return true;
    }

ErrorExit:
		
	// We need to stop the port objects...
		
	USBError(1,"AppleUSBHub[%p]::start Aborting startup: error 0x%x", this, err);
	if ( _device && _device->isOpen(this) )
		_device->close(this);
	
	StopPorts();
	
	stop(_device);
	
	if ( _timerSource )
	{
		if ( _workLoop )
			_workLoop->removeEventSource(_timerSource);
		
		_timerSource->release();
		_timerSource = NULL;
	}
	
    if (_gate)
    {
		if (_workLoop)
			_workLoop->removeEventSource(_gate);
	    
		_gate->release();
		_gate = NULL;
    }
	
    if ( _workLoop )
    {
        _workLoop->release();
        _workLoop = NULL;
    }
	
    _inStartMethod = false;
    DecrementOutstandingIO();
	LowerPowerState();
    return false;
}



void 
AppleUSBHub::stop(IOService * provider)
{

    if (_buffer) 
    {
        _buffer->release();
		_buffer = NULL;
    }
	
	if (_hsHub)
	{
		if (_bus)
			_bus->RemoveHSHub(_address);
	}
	
    if(_hubInterface) 
    {
        _hubInterface->close(this);
        _hubInterface->release();
        _hubInterface = NULL;
    }
    
    if (_workThread)
    {
        thread_call_cancel(_workThread);
        thread_call_free(_workThread);
    }
    
    if (_resetPortZeroThread)
    {
        thread_call_cancel(_resetPortZeroThread);
        thread_call_free(_resetPortZeroThread);
    }

    if (_hubDeadCheckThread)
    {
        thread_call_cancel(_hubDeadCheckThread);
        thread_call_free(_hubDeadCheckThread);
    }

    if (_clearFeatureEndpointHaltThread)
    {
        thread_call_cancel(_clearFeatureEndpointHaltThread);
        thread_call_free(_clearFeatureEndpointHaltThread);
    }

	if (_workLoop)
		_workLoop->removeEventSource(_gate);

	if ( _workLoop )
		_workLoop->removeEventSource(_timerSource);
	
    USBLog(3, "AppleUSBHub[%p]::stop - calling PMstop", this);
	PMstop();
	
    USBLog(3, "AppleUSBHub[%p]::stop - calling super::stop", this);
    super::stop(provider);
}



IOReturn 
AppleUSBHub::message( UInt32 type, IOService * provider,  void * argument )
{
    IOReturn						err = kIOReturnSuccess;
    IOUSBHubPortStatus				status;
    IOUSBHubPortReEnumerateParam *	params ;
    IOUSBHubPortClearTTParam *      ttParams;
    
	USBLog(3, "AppleUSBHub[%p]::message(%s) - isInactive(%s) _myPowerState(%d) _powerStateChangingTo(%d)", this, HubMessageToString(type), isInactive() ? "true" : "false", (int)_myPowerState, (int)_powerStateChangingTo);
	
    switch ( type )
    {
		case kIOUSBMessageHubIsDeviceConnected:
		
			// If we are in the process of terminating, or if we have determined that the hub is dead, then
			// just return as if the device was unplugged.  The hub itself is going away or is already resetting,
			// so it any device that was connected will not be anymore.
			//
			if ( isInactive() || _hubIsDead )
			{
				USBLog(3,"AppleUSBHub[%p] : got kIOUSBMessageHubIsDeviceConnected while isInactive() or _hubIsDead", this);
				err = kIOReturnNoDevice;
				break;
			}
			
			// Get the status for the port.  Note that the argument passed into this method is the port number.  If we get an
			// error from the call, then that means that our hub is not 100% so the device is probably not connected.  Otherwise
			// check the kHubPortConnection bit to see whether there is a device connected to the port or not
			//
			USBLog(5, "AppleUSBHub[%p]::message(kIOUSBMessageHubIsDeviceConnected) - calling IncrementOutstandingIO", this);
			IncrementOutstandingIO();
			err = GetPortStatus(&status, * (UInt32 *) argument );
			if ( err != kIOReturnSuccess )
			{
				err = kIOReturnNoDevice;
			}
			else
			{
				USBLog(5,"AppleUSBHub[%p]::kIOUSBMessageHubIsDeviceConnected - port %ld - status(%8x)/change(%8x)", this, * (UInt32 *) argument, status.statusFlags, status.changeFlags);
				if ( (status.statusFlags & kHubPortConnection) && !(status.changeFlags & kHubPortConnection) )
					err = kIOReturnSuccess;
				else
					err = kIOReturnNoDevice;
			}
			USBLog(5, "AppleUSBHub[%p]::message(kIOUSBMessageHubIsDeviceConnected) - calling DecrementOutstandingIO", this);
			DecrementOutstandingIO();
			break;
		
		case kIOUSBMessageHubSuspendPort:
		case kIOUSBMessageHubResumePort:
		case kIOUSBMessageHubResetPort:
			USBLog(5, "AppleUSBHub[%p]::message(%s) - calling EnsureUsability", this, HubMessageToString(type));
			EnsureUsability();
			err = DoPortAction( type, * (UInt32 *) argument, 0 );
			break;
		
		case kIOUSBMessageHubPortClearTT:
			ttParams = (IOUSBHubPortClearTTParam *) argument;
			err = DoPortAction( type, ttParams->portNumber, ttParams->options );
			break;
			
		case kIOUSBMessageHubSetPortRecoveryTime:
		case kIOUSBMessageHubReEnumeratePort:
			params = (IOUSBHubPortReEnumerateParam *) argument;
			err = DoPortAction( type, params->portNumber, params->options );
			break;
			
		case kIOMessageServiceIsTerminated: 	
			USBLog(3,"AppleUSBHub[%p] : Received kIOMessageServiceIsTerminated - ignoring", this);
			break;
			
			
		case kIOUSBMessagePortHasBeenReset:
			if ( isInactive() )
			{
				USBLog(5,"AppleUSBHub[%p] : got kIOUSBMessagePortHasBeenReset while isInactive() or _hubIsDead", this);
				err = kIOReturnSuccess;
				break;
			}
		
			// Should we do something here if we get an error?
			//
			if (!_inStartMethod)
			{
				_inStartMethod = true;
				USBLog(5, "AppleUSBHub[%p]::message(kIOUSBMessagePortHasBeenReset) - calling RaisePowerState and IncrementOutstandingIO", this);
				RaisePowerState();
				IncrementOutstandingIO();		// make sure we don't close until start is done
				USBLog(3, "AppleUSBHub[%p]  Received kIOUSBMessagePortHasBeenReset -- reconfiguring hub", this);
				
				// Abort any transactions (should not have any pending at this point)
				//
				if ( _interruptPipe )
				{
					_interruptPipe->Abort();
					_interruptPipe = NULL;
				}
				
				// When we reconfigure the hub, we'll recreate the hubInterface, so let's tear it down.
				//
				if(_hubInterface) 
				{
					_hubInterface->close(this);
					_hubInterface->release();
					_hubInterface = NULL;
				}
				
				// Reconfigure our Hub. 
				err = ConfigureHub();
				if ( err ) 
				{
					USBLog(3, "AppleUSBHub[%p] Reconfiguring hub returned: 0x%x", this, err);
				}
				else
				{
					USBLog(1, "[%p] (Reset) USB Generic Hub @ %d (0x%lx)", this, _address, _locationID);
				}
				
				_inStartMethod = false;
				USBLog(5, "AppleUSBHub[%p]::message(kIOUSBMessagePortHasBeenReset) - calling DecrementOutstandingIO", this);
				DecrementOutstandingIO();
				LowerPowerState();
				
				// Only rearm the interrupt if we successfully configured the
				// hub
				if ( err == kIOReturnSuccess)
				{
					makeUsable();
				}
			}
			USBLog(3, "-AppleUSBHub[%p]  Received kIOUSBMessagePortHasBeenReset -- finished reconfiguring hub", this);
			
			break;
			
		case kIOUSBMessagePortHasBeenResumed:
		case kIOUSBMessagePortWasNotSuspended:
			_portSuspended = false;
			_abortExpected = false;
			USBLog(5, "AppleUSBHub[%p]: received kIOUSBMessagePortHasBeenResumed or kIOUSBMessagePortWasNotSuspended (%p) - _myPowerState(%d) - ensuring usability", this, (void*)type, (int)_myPowerState);
			_needInterruptRead = true;
			EnsureUsability();
			break;
		
		default:
			break;
		
    }
    
    return err;
}



bool 
AppleUSBHub::finalize(IOOptionBits options)
{
    return(super::finalize(options));
}



bool 	
AppleUSBHub::requestTerminate( IOService * provider, IOOptionBits options )
{
	int			retries = 600;
	
    USBLog(3, "AppleUSBHub[%p]::requestTerminate - _myPowerState(%d) _powerStateChangingTo(%d)", this, (int)_myPowerState, (int)_powerStateChangingTo);
	// let's be ON to terminate
	if ((_myPowerState != kIOUSBHubPowerStateOn) || (_powerStateChangingTo != kIOUSBHubPowerStateStable))
		changePowerStateTo(kIOUSBHubPowerStateOn);

	while ((_myPowerState != kIOUSBHubPowerStateOn) && (_powerStateChangingTo != kIOUSBHubPowerStateStable) && (retries-- > 0))
	{
		USBLog(3, "AppleUSBHub[%p]::requestTerminate - still waiting - _myPowerState(%d) _powerStateChangingTo(%d) retries left(%d)", this, (int)_myPowerState, (int)_powerStateChangingTo, retries);
		IOSleep(100);
	}
    return super::requestTerminate(provider, options);
}



bool
AppleUSBHub::willTerminate( IOService * provider, IOOptionBits options )
{
    IOReturn				err;
    int						portIndex, portNum;
    AppleUSBHubPort *		port;
    
    USBLog(3, "AppleUSBHub[%p]::willTerminate isInactive = %d", this, isInactive());
	
    if ( _interruptPipe )
    {
		err = _interruptPipe->Abort();
        if ( err != kIOReturnSuccess )
        {
            USBLog(1, "AppleUSBHub[%p]::willTerminate interruptPipe->Abort returned 0x%x", this, err);
        }
    }
	
    // JRH 09/19/2003 rdar://problem/3290312
    // make sure that none of our ports has the dev zero lock held. if they do, it is safe to go ahead
    // and release it since the hub is now gone (we are terminating)
    if ( _ports)
    {
        for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
        {
			portNum = portIndex + 1;
            port = _ports[portIndex];
            if (port)
            {
                if (port->_devZero)
                {
                    USBLog(1, "AppleUSBHub[%p]::willTerminate - port %d had the dev zero lock", this, portNum);
                }
                port->ReleaseDevZeroLock();
				if (port->_portPMState == usbHPPMS_pm_suspended)
				{
 					IOUSBControllerV3		*v3Bus = NULL;

					USBLog(5, "AppleUSBHub[%p]::willTerminate - port %d was suspended by the power manager - re-enabling the endpoints", this, portNum);

					if (_device)
						v3Bus = OSDynamicCast(IOUSBControllerV3, _bus);
					
					if (v3Bus && port->_portDevice)
					{
						USBLog(5, "AppleUSBHub[%p]::StartPorts - Enabling endpoints for device at address (%d)", this, (int)port->_portDevice->GetAddress());
						err = v3Bus->EnableAddressEndpoints(port->_portDevice->GetAddress(), true);
						if (err)
						{
							USBLog(5, "AppleUSBHub[%p]::StartPorts - EnableAddressEndpoints returned (%p)", this, (void*)err);
						}
					}
					port->_portPMState = usbHPPMS_active;
				}
            }
        }
    }
	
    // We are going to be terminated, so clean up! Make sure we don't get any more status change interrupts.
    // Note that if we are terminated before we set up our interrupt pipe, then we better not call it!
    //
    if (_timerSource) 
    {
		_timerSource->cancelTimeout();
    }
    
    USBLog(3, "AppleUSBHub[%p]::willTerminate - calling super::willTerminate", this);
    return super::willTerminate(provider, options);
}


bool
AppleUSBHub::didTerminate( IOService * provider, IOOptionBits options, bool * defer )
{
    USBLog(3, "AppleUSBHub[%p]::didTerminate isInactive[%s] _outstandingIO[%d]", this, isInactive() ? "true" : "false", (int)_outstandingIO);
    
    if (!_outstandingIO && (_powerStateChangingTo == kIOUSBHubPowerStateStable) && IOLockTryLock(_doPortActionLock))
    {
		USBLog(5, "AppleUSBHub[%p]::didTerminate - closing _device(%p)", this, _device);
		// Stop/close all ports, deallocate our ports
		//
		StopPorts();
		IOLockUnlock(_doPortActionLock);
		_device->close(this);
    }
    else
    {
		USBLog(5, "AppleUSBHub[%p]::didTerminate - _device(%p) - setting needToClose - _outstandingIO(%d) _powerStateChangingTo(%d)", this, _device, (int)_outstandingIO, (int)_powerStateChangingTo);
		_needToClose = true;
    }
    return super::didTerminate(provider, options, defer);
}


bool
AppleUSBHub::terminate( IOOptionBits options )
{
    USBLog(3, "AppleUSBHub[%p]::terminate isInactive = %d", this, isInactive());
    return super::terminate(options);
}


void
AppleUSBHub::free( void )
{
    USBLog(6, "AppleUSBHub[%p]::free isInactive = %d", this, isInactive());
	
	if (_doPortActionLock) 
	{
		IOLockFree(_doPortActionLock);
		_doPortActionLock = NULL;
	}

	if (_timerSource)
    {
        _timerSource->release();
        _timerSource = NULL;
    }
	
    if (_gate)
    {
        _gate->release();
        _gate = NULL;
    }
    
	if (_workLoop)
	{
		_workLoop->release();
		_workLoop = NULL;
	}
	
    super::free();
}



bool
AppleUSBHub::terminateClient( IOService * client, IOOptionBits options )
{
    USBLog(3, "AppleUSBHub[%p]::terminateClient isInactive = %d", this, isInactive());
    return super::terminateClient(client, options);
}



IOReturn
AppleUSBHub::powerStateWillChangeTo ( IOPMPowerFlags capabilities, unsigned long stateNumber, IOService* whatDevice)
{
	USBLog(5, "AppleUSBHub[%p]::powerStateWillChangeTo - from State(%d) to state(%d) _needInterruptRead (%s) _outstandingIO(%d) isInactive(%s)", this, (int)_myPowerState, (int)stateNumber, _needInterruptRead ? "true" : "false", (int)_outstandingIO, isInactive() ? "true" : "false");
	return super::powerStateWillChangeTo( capabilities, stateNumber, whatDevice);
}



IOReturn		
AppleUSBHub::powerStateDidChangeTo ( IOPMPowerFlags capabilities, unsigned long stateNumber, IOService* whatDevice)
{	
	unsigned long	oldState = _myPowerState;
	IOReturn		ret;
	
	USBLog(5, "AppleUSBHub[%p]::+powerStateDidChangeTo - from State(%d) to state(%d) _needInterruptRead (%s)", this, (int)oldState, (int)stateNumber, _needInterruptRead ? "true" : "false");
	ret =  super::powerStateDidChangeTo( capabilities, stateNumber, whatDevice);

	// this needs to be done here instead of in powerChangeDone, because powerChangeDone won't get called until our children are all up, and 
	// one of our children might need an interrupt read to occur, which won't occur until this Decrement occurs
	if (oldState != stateNumber)
		DecrementOutstandingIO();				// this is from the increment in powerStateWillChangeTo
		
	USBLog(5, "AppleUSBHub[%p]::-powerStateDidChangeTo - from State(%d) to state(%d) _needInterruptRead (%s) ret[%p]", this, (int)oldState, (int)stateNumber, _needInterruptRead ? "true" : "false", (void*)ret);
	return ret;
}



void
AppleUSBHub::powerChangeDone ( unsigned long fromState)
{
	USBLog((fromState == _myPowerState) ? 7 : 5, "AppleUSBHub[%p]::powerChangeDone - device(%s) from State(%d) to state(%d) _needInterruptRead (%s)", this, _device->getName(), (int)fromState, (int)_myPowerState, _needInterruptRead ? "true" : "false");
	super::powerChangeDone(fromState);
	
	if (isInactive() && _needToClose)
	{
		// tickle the outstanding io - just in case
		IncrementOutstandingIO();
		DecrementOutstandingIO();
	}
	
	// Check flag and call reset and then turn on again and do override
	if ( _needToCallResetDevice and _myPowerState == kIOUSBHubPowerStateOff)
	{
		_needToCallResetDevice = false;
		USBLog(1, "AppleUSBHub[%p]::powerChangeDone - calling ResetDevice()", this);
		// Ask our device to reset our port
		//
		IOReturn err = _device->ResetDevice();
		if ( err != kIOReturnSuccess)
		{
			USBLog(1, "AppleUSBHub[%p]::powerChangeDone - ResetDevice returned 0x%x", this, err);
		}
		
		// Now, make sure that our hub goes back to the on state
		USBLog(1, "AppleUSBHub[%p]::powerChangeDone - calling changePowerStateToPriv(kIOUSBHubPowerStateOn)", this);
		changePowerStateToPriv(kIOUSBHubPowerStateOn);
		powerOverrideOffPriv();
		
	}
}



#pragma mark �������� Configuration ���������
IOReturn
AppleUSBHub::ConfigureHub()
{
    IOReturn							err = kIOReturnSuccess;
    IOUSBFindInterfaceRequest			req;
    const IOUSBConfigurationDescriptor	*cd;
	OSBoolean							*expressCardCantWakeRef;

    // Reset some of our variables that so that when we reconfigure due to a reset
    // we don't reuse old values
    //
    _busPowerGood = false;
    _powerForCaptive = 0;
    _numCaptive = 0;
	_retryCount = kHubDriverRetryCount;
	
    // Find the first config/interface
    if (_device->GetNumConfigurations() < 1)
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub No hub configurations", this);
        err = kIOReturnNoResources;		// Need better error
        goto ErrorExit;
    }

    // set the configuration to the first config
    cd = _device->GetFullConfigurationDescriptor(0);
    if (!cd)
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub No config descriptor", this);
        err = kIOUSBConfigNotFound;
        goto ErrorExit;
    }

    err = _device->SetConfiguration(this, cd->bConfigurationValue, false);
    
    if (err)
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub SetConfiguration failed. Error 0x%x", this, err);
        goto ErrorExit;
    }
        
    // Set the remote wakeup feature if it's supported
    //
    if (cd->bmAttributes & kUSBAtrRemoteWakeup)
    {
        USBLog(3,"AppleUSBHub[%p]::ConfigureHub Setting kUSBFeatureDeviceRemoteWakeup for Hub device (%p)", this, _device);
        err = _device->SetFeature(kUSBFeatureDeviceRemoteWakeup);
        if ( err)
            USBError(1,"AppleUSBHub[%p]::ConfigureHub SetFeature(kUSBFeatureDeviceRemoteWakeup) failed. Error 0x%x", this, err);
    }

	// See if this is an express card device which would disconnect on sleep (thus waking everytime)
	//
	expressCardCantWakeRef = OSDynamicCast( OSBoolean, _device->getProperty(kUSBExpressCardCantWake) );
	if ( expressCardCantWakeRef && expressCardCantWakeRef->isTrue() )
	{
		USBLog(3, "%s[%p](%s) found an express card device which will disconnect across sleep", getName(), this, _device->getName() );
		_device->GetBus()->retain();
		_device->GetBus()->message(kIOUSBMessageExpressCardCantWake, this, _device);
		_device->GetBus()->release();
	}

	// Find the interface for our hub -- there's only one
    //
    req.bInterfaceClass = kUSBHubClass;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    _hubInterface = _device->FindNextInterface(NULL, &req);
	if (_hubInterface  == 0)
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub no interface found, trying again", this);
		
		IOSleep(100);
		req.bInterfaceClass = kUSBHubClass;
		req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
		req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
		req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
		_hubInterface = _device->FindNextInterface(NULL, &req);
		if (_hubInterface  == 0)
		{
			USBError(1,"AppleUSBHub[%p]::ConfigureHub no interface found", this);
			err = kIOUSBInterfaceNotFound;
			goto ErrorExit;
		}
    }
    
    _hubInterface->retain();
    
    
    _busPowered = (cd->bmAttributes & kUSBAtrBusPowered) ? TRUE : FALSE;	//FIXME
    _selfPowered = (cd->bmAttributes & kUSBAtrSelfPowered) ? TRUE : FALSE;

    if( !(_busPowered || _selfPowered) )
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub illegal device config - no power", this);
        err = kIOReturnNoPower;		// Need better error code here.
        goto ErrorExit;
    }

    // Get the hub descriptor
    if ( (err = GetHubDescriptor(&_hubDescriptor)) )
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub could not get hub descriptor (0x%x)", this, err);
        goto ErrorExit;
    }
    
    if(_hubDescriptor.numPorts < 1)
    {
        USBLog(1,"AppleUSBHub[%p]::ConfigureHub there are no ports on this hub", this);
    }
    if(_hubDescriptor.numPorts > 7)
    {
        USBLog(3,"AppleUSBHub[%p]::ConfigureHub there are an awful lot of ports (%d) on this hub", this, _hubDescriptor.numPorts);
    }
    _readBytes = ((_hubDescriptor.numPorts + 1) / 8) + 1;
    
    // Setup for reading status pipe
    _buffer = IOBufferMemoryDescriptor::withCapacity(_readBytes, kIODirectionIn);

    if (!_hubInterface->open(this))
    {
        USBError(1," AppleUSBHub[%p]::ConfigureHub could not open hub interface", this);
        err = kIOReturnNotOpen;
        goto ErrorExit;
    }
    
    // after opening the interface, but before we get the pipe, we need to see if this is a 2.0
    // capabale hub, and if so, we need to set the multiTT status if possible
    _multiTTs = false;
    _hsHub = false;
    
    if (_device->GetbcdUSB() >= 0x200)
    {
		if (_bus)
		{
			switch (_device->GetProtocol())
			{
			case 0:
				USBLog(5, "AppleUSBHub[%p]::ConfigureHub - found FS/LS only hub", this);
				break;
				
			case 1:
				USBLog(5, "AppleUSBHub[%p]::ConfigureHub - found single TT hub", this);
				_bus->AddHSHub(_address, 0);
				_hsHub = true;
				break;
				
			case 2:
				USBLog(5, "AppleUSBHub[%p]::ConfigureHub - found multi TT hub", this);
				_hsHub = true;

				if ((err = _hubInterface->SetAlternateInterface(this, 1))) 		// pick the multi-TT setting
				{
					USBError(1, "AppleUSBHub[%p]::ConfigureHub - err (%x) setting alt interface", this, err);
					_bus->AddHSHub(_address, 0);
				}
				else
					_bus->AddHSHub(_address, kUSBHSHubFlagsMultiTT);
				
				_multiTTs = true;
				break;
				
			default:
				USBError(1, "AppleUSBHub[%p]::ConfigureHub - unknown protocol (%d)", this, _device->GetProtocol());
				break;
			}
		}
		else
		{
			USBLog(5, "AppleUSBHub[%p]::ConfigureHub - not on a V2 controller", this);
		}
    }
    

    IOUSBFindEndpointRequest request;
    request.type = kUSBInterrupt;
    request.direction = kUSBIn;
    _interruptPipe = _hubInterface->FindNextPipe(NULL, &request);

    if(!_interruptPipe)
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub could not find interrupt pipe", this);
        err = kIOUSBNotEnoughPipesErr;		// Need better error code here.
        goto ErrorExit;
    } 
    
    // prepare the ports
    UnpackPortFlags();
    CountCaptivePorts();
    err = CheckPortPowerRequirements();
    if ( err != kIOReturnSuccess )
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub CheckPortPowerRequirements failed with 0x%x", this, err);
        goto ErrorExit;
    }

    err = AllocatePortMemory();
    if ( err != kIOReturnSuccess )
    {
        USBError(1,"AppleUSBHub[%p]::ConfigureHub AllocatePortMemory failed with 0x%x", this, err);
        goto ErrorExit;
    }
    
    if (_hsHub)
    {
		// with a HS hub, we will put a property in our own object specifying the number
		// of TTs in the hub. 
		if (!_multiTTs)
			setProperty("High Speed", (unsigned long long)1, 8);		// 8 bits
		else
			setProperty("High Speed", (unsigned long long)_hubDescriptor.numPorts, 8);	// 8 bits
    }
    
	// Set the UserClient for all hubs, hs and full speed
	setProperty("IOUserClientClass", "AppleUSBHSHubUserClient");

	_hubIsDead = FALSE;

ErrorExit:

    return err;
}


/**********************************************************************
 **
 ** HUB FUNCTIONS
 **
 **********************************************************************/
void 
AppleUSBHub::UnpackPortFlags(void)
{
    int i;

    int numFlags = ((_hubDescriptor.numPorts + 1) / 8) + 1;
    for(i = 0; i < numFlags; i++)
    {
        _hubDescriptor.pwrCtlPortFlags[i] = _hubDescriptor.removablePortFlags[numFlags+i];
        _hubDescriptor.removablePortFlags[numFlags+i] = 0;
    }
}



void 
AppleUSBHub::CountCaptivePorts(void)
{
    int 		portMask = 2;
    int 		portByte = 0;
    int			currentPort;


    for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
    {
        /* determine if the port is a captive port */
        if ((_hubDescriptor.removablePortFlags[portByte] & portMask) != 0)
            _numCaptive++;		// save this for power calculations

        portMask <<= 1;
        if(portMask > 0x80)
        {
            portMask = 1;
            portByte++;
        }
    }
}



/*
                ExtPower   ExtPower
                Good       off

Bus  Self

0     0     	Illegal config
1     0     	Always 100mA per port
0     1     	500mA     0 (dead)
1     1     	500       100
*/

IOReturn 
AppleUSBHub::CheckPortPowerRequirements(void)
{
    IOReturn	err = kIOReturnSuccess;
    /* Note hub current in units of 1mA, everything else in units of 2mA */
    UInt32	hubPower = _hubDescriptor.hubCurrent/2;
    UInt32	busPower = _device->GetBusPowerAvailable();
    UInt32	powerAvailForPorts = 0;
    UInt32	powerNeededForPorts = 0;
    bool	startExternal;

    do
    {
        if (hubPower > busPower)
        {
            // Don't put up an alert here.  The Adaptec 2.0 Hub claims that it needs 250mA. We will catch this later
            //
            USBLog(3, "AppleUSBHub [%p] Hub claims to need more power (%ld > %ld) than available", this, hubPower, busPower);
            _busPowerGood = false;
            _powerForCaptive = 0;
        }
        else
        {
            powerAvailForPorts = busPower - hubPower;
            /* we minimally need make available 100mA per non-captive port */
            powerNeededForPorts = (_hubDescriptor.numPorts - _numCaptive) * kUSB100mA;
            _busPowerGood = (powerAvailForPorts >= powerNeededForPorts);

            if(_numCaptive > 0)
            {
                if(_busPowerGood)
                    _powerForCaptive =
                        (powerAvailForPorts - powerNeededForPorts) / _numCaptive;
                else
                    _powerForCaptive = powerAvailForPorts / _numCaptive;
            }

            if( (_errataBits & kErrataCaptiveOKBit) != 0)
                _powerForCaptive = kUSB100mAAvailable;
        }
        
        _selfPowerGood = false;
        
        if (_selfPowered)
        {
            // Check the status of the power source
            //
            USBStatus	status = 0;
            IOReturn	localErr;

            _powerForCaptive = kUSB100mAAvailable;

            localErr = _device->GetDeviceStatus(&status);
            if ( localErr != kIOReturnSuccess )
            {
                err = localErr;
                break;
            }
            
            status = USBToHostWord(status);
            _selfPowerGood = ((status & 1) != 0);	// FIXME 1?
        }

        if(_selfPowered && _busPowered)
        {
            /* Dual power hub */
            
            if(_selfPowerGood)
            {
                USBLog(3,"AppleUSBHub[%p]::CheckPowerPowerRequirements - Hub attached - Self/Bus powered, power supply good", this);
				if (!_isRootHub && !_dontAllowSleepPower)
				{
					USBLog(3,"AppleUSBHub[%p]::CheckPowerPowerRequirements -  setting Extra Power in sleep to %d", this, (int)(_hubDescriptor.numPorts * kUSB500mAAvailable * 2));
					// self powered hubs can provide extra power in sleep - 500 ma per port
					_device->setProperty(kAppleExtraPowerInSleep, _hubDescriptor.numPorts * kUSB500mAAvailable * 2, 32);
				}
			}
            else
            {
                USBLog(3,"AppleUSBHub[%p] Hub attached - Self/Bus powered, no external power", this);
            }
        }
        else
        {
            /* Single power hub */
            if(_selfPowered)
            {
                if(_selfPowerGood)
                {
                    USBLog(3,"AppleUSBHub[%p]::CheckPowerPowerRequirements -  Hub attached - Self powered, power supply good", this);

					if (!_isRootHub && !_dontAllowSleepPower)
					{
						USBLog(3,"AppleUSBHub[%p]::CheckPowerPowerRequirements -  setting Extra Power in sleep to %d", this, (int)_hubDescriptor.numPorts * kUSB500mAAvailable * 2);
						// self powered hubs can provide extra power in sleep - 500 ma per port
						_device->setProperty(kAppleExtraPowerInSleep, _hubDescriptor.numPorts * kUSB500mAAvailable * 2, 32);
					}
              }
                else
                {
                    USBLog(3,"AppleUSBHub[%p] Hub attached - Self powered, no external power", this);
                }
            }
            else
            {
                USBLog(3,"AppleUSBHub[%p] Hub attached - Bus powered", this);
            }

        }
#if defined (__i386__)
		if (_isRootHub && (_device->GetHubCharacteristics() & kIOUSBHubDeviceCanSleep))
		{
			_device->setProperty(kAppleExtraPowerInSleep, _hubDescriptor.numPorts * kUSB500mAAvailable * 2, 32);
		}
#endif
        startExternal = (_busPowerGood || _selfPowerGood);
        if( !startExternal )
        {	/* not plugged in or bus powered on a bus powered hub */
            err = kIOReturnNoPower;
            _device->DisplayUserNotification(kUSBNotEnoughPowerNotificationType);
            IOLog("USB Low Power Notice:  The hub \"%s\" cannot be used because there is not enough power for all its ports\n", _device->getName());
            USBLog(1,"AppleUSBHub[%p]: insufficient power to turn on ports", this);
            if(!_busPowered)
            {
                /* may be able to turn on compound devices */
                break;	/* Now what ?? */
            }
        }
    } while (false);

    return err;
}



IOReturn 
AppleUSBHub::AllocatePortMemory(void)
{
    AppleUSBHubPort 	*port;
    UInt32		power;
    UInt32 		portMask = 2;
    UInt32 		portByte = 0;
    UInt32		currentPort;
    bool		captive;


    _ports = (AppleUSBHubPort **) IOMalloc(sizeof(AppleUSBHubPort *) * _hubDescriptor.numPorts);

    if (!_ports)
        return kIOReturnNoMemory;

    for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
    {
        /* determine if the port is a captive port */
        if ((_hubDescriptor.removablePortFlags[portByte] & portMask) != 0)
        {
            power = _selfPowerGood ? (UInt32)kUSB500mAAvailable : _powerForCaptive;
            captive = true;
        }
        else
        {
            power = _selfPowerGood ? kUSB500mAAvailable : kUSB100mAAvailable;
            captive = false;
        }

        port = new AppleUSBHubPort;
        if (port->init(self, currentPort, power, captive) != kIOReturnSuccess)
        {
            port->release();
            _ports[currentPort-1] = NULL;
        }
        else
            _ports[currentPort-1] = port;

        portMask <<= 1;
        if(portMask > 0x80)
        {
            portMask = 1;
            portByte++;
        }
    }
    
    return kIOReturnSuccess;
}



#pragma mark ���� Power Management ����
IOReturn		
AppleUSBHub::HubPowerChange(unsigned long powerStateOrdinal)
{
	IOReturn		err = kIOReturnSuccess;
	UInt32			retries;
	IOReturn		ret = kIOPMAckImplied;
	
	USBLog(5, "AppleUSBHub[%p]::HubPowerChange - powerStateOrdinal(%d) _needInterruptRead(%s) isInactive(%s)", this, (int)powerStateOrdinal, _needInterruptRead ? "true" : "false", isInactive() ? "true" : "false");
	
	if (_myPowerState != powerStateOrdinal)
		IncrementOutstandingIO();				// this will be decremented in powerStateDidChangeTo
	
	switch (powerStateOrdinal)
	{
		case kIOUSBHubPowerStateOn:
			if (_portSuspended && _device)
			{
				USBLog(5, "AppleUSBHub[%p]::HubPowerChange - hub going to ON - my port is suspended, waking up onThread(%s)", this, _workLoop->onThread() ? "true" : "false");
				_device->SuspendDevice(false);
				retries = 20;
				do {
					IOSleep(20);
				} while (_portSuspended && (retries-- > 0));
				IOSleep(_hubResumeRecoveryTime);			// 10 more ms for the recovery time before I start talking to my device
				if (retries == 0)
				{
					USBError(1, "AppleUSBHub[%p]::HubPowerChange - hub going to ON - my port is suspended, could not wake it up!  onThread(%s)", this, _workLoop->onThread() ? "true" : "false");
				}
			}
			if (!_ports)
			{
				err = AllocatePortMemory();
				if (err)
				{
					USBError(1,"AppleUSBHub[%p]::HubPowerChange - AllocatePortMemory failed with %p", this, (void*)err);
				}
			}
			// power ON (or unsuspend) the ports
			if (_myPowerState < kIOUSBHubPowerStateLowPower)
			{
				err = StartPorts();
				if ( err != kIOReturnSuccess )
				{
					USBError(1,"AppleUSBHub[%p]::HubPowerChange - StartPorts failed with 0x%x", this, err);
					break;
				}
			}
			// Start the timeout Timer
			if (_timerSource)
			{
				_timerSource->setTimeoutMS(kWatchdogTimerPeriod); 
			}	
			
			if ( !_isRootHub && _interruptPipe)
				_interruptPipe->ClearPipeStall(true);

			_abortExpected = false;
			_needInterruptRead = true;
			break;
			
		case kIOUSBHubPowerStateLowPower:

			if ( _dontAllowLowPower )
			{
				USBLog(1, "AppleUSBHub[%p]::HubPowerChange - hub going to doze - but this hub does NOT allow it", this);
				break;
			}
			if (_myPowerState > kIOUSBHubPowerStateLowPower)				// don't do anything if we are coming from a lower state
			{
				if (_portSuspended)
				{
					USBLog(1, "AppleUSBHub[%p]::HubPowerChange ((hub @ 0x%lx) - hub going to doze - my port is suspended! UNEXPECTED", this, _locationID);
					break;
				}
				if (!HubAreAllPortsDisconnectedOrSuspended())
				{
					USBLog(1, "AppleUSBHub[%p]::HubPowerChange (hub @ 0x%lx) - hub going to doze - some port is not suspended! UNEXPECTED", this, _locationID);
				}
				else
				{
					// kill my interrupt pipe, since my upstream hub will suspend me
					if ( _interruptPipe )
					{
						USBLog(5, "AppleUSBHub[%p]::HubPowerChange - aborting pipe", this);
						_abortExpected = true;
						_interruptPipe->Abort();
						USBLog(5, "AppleUSBHub[%p]::HubPowerChange - done aborting pipe", this);
					}
					// Proceed to suspend the port
					// We need to suspend our port.  If we have I/O pending, set a flag that tells the interrupt handler
					// routine that we don't need to rearm the read.
					//
					// Now, call in to suspend the port
					if ( _device && !_isRootHub)
					{
						USBLog(5, "AppleUSBHub[%p]::HubPowerChange - calling SuspendDevice", this);
						err = _device->SuspendDevice(true);
						USBLog(5, "AppleUSBHub[%p]::HubPowerChange - done with SuspendDevice", this);
						if ( err == kIOReturnSuccess )
						{
							_portSuspended = true;
						}
						else
						{
							USBLog(1, "AppleUSBHub[%p]::HubPowerChange SuspendDevice returned %p", this, (void*)err );
						}
					}
					else
					{
						USBLog(5, "AppleUSBHub[%p]::HubPowerChange _isRootHub or _device was NULL", this );
					}
				}
			}
			_needInterruptRead = false;
			break;
			
		case kIOUSBHubPowerStateSleep:
			if (_hubIsDead)
			{
				USBLog(1, "AppleUSBHub[%p]::HubPowerChange - hub is dead, so just acknowledge the HubPowerChange", this);
				break;
			}
		
			if (!(_device->GetHubCharacteristics() & kIOUSBHubDeviceCanSleep))
			{
				USBLog(1, "AppleUSBHub[%p]::HubPowerChange - hub doesn't support sleep, don't do anything on Suspend", this);
				break;
			}
			// first kill the timer so we don't notice that the ports are suspended
			if (_timerSource) 
			{
				_timerSource->cancelTimeout();
			}
			// only do this stuff if I am not already suspended
			if (!_portSuspended)
			{
				// I need to suspend each of my downstream ports
				err = SuspendPorts();
				if ( err != kIOReturnSuccess )
				{
					USBError(1,"AppleUSBHub[%p]::HubPowerChange - SuspendPorts failed with 0x%x", this, err);
					break;
				}
				// now kill my interrupt pipe, since my upstream hub will suspend me
				if ( _interruptPipe )
				{
					USBLog(5, "AppleUSBHub[%p]::HubPowerChange - aborting pipe!!", this);
					_abortExpected = true;
					_interruptPipe->Abort();
				}
				// Proceed to suspend the upstream port
				// We need to suspend our port.  If we have I/O pending, set a flag that tells the interrupt handler
				// routine that we don't need to rearm the read.
				//
				// Now, call in to suspend the port
				if ( _device && !_isRootHub)
				{
					USBLog(5, "AppleUSBHub[%p]::HubPowerChange - calling SuspendDevice", this);
					err = _device->SuspendDevice(true);
					USBLog(5, "AppleUSBHub[%p]::HubPowerChange - done with SuspendDevice", this);
					if ( err == kIOReturnSuccess )
					{
						_portSuspended = true;
					}
					else
					{
						USBLog(1, "AppleUSBHub[%p]::HubPowerChange SuspendDevice returned %p", this, (void*)err );
					}
				}
				else
				{
					USBLog(5, "AppleUSBHub[%p]::HubPowerChange _isRootHub or _device was NULL - not suspending", this );
				}
			}
			else
			{
				USBLog(5, "AppleUSBHub[%p]::HubPowerChange - device already suspended - no need to do it again", this );
			}
			_needInterruptRead = false;
			break;
			
		case kIOUSBHubPowerStateOff:
		case kIOUSBHubPowerStateRestart:
			// Stop/close all ports, deallocate our ports
			//			
			StopPorts();
			
			_needInterruptRead = false;
			break;
		
		default:
			USBError(1, "AppleUSBHub[%p]::HubPowerChange - unknown ordinal (%d)", this, (int)powerStateOrdinal);
	}
	USBLog(5, "AppleUSBHub[%p]::HubPowerChange - done _needInterruptRead(%s) _outstandingResumes(%d)", this, _needInterruptRead ? "true" : "false", (int)_outstandingResumes);
	if (_outstandingResumes)
	{
		USBLog(5, "AppleUSBHub[%p]::HubPowerChange - with outstanding resumes - spawning thread and returning kIOPMWillAckLater", this);
		_needToAckSetPowerState = true;
		thread_call_enter(_waitForPortResumesThread);
		ret = kIOPMWillAckLater;
	}
	return ret;
}



bool	
AppleUSBHub::HubAreAllPortsDisconnectedOrSuspended()
{
    UInt32					portIndex, portNum;
    IOUSBHubPortStatus		portStatus;
	IOReturn				kr;
    AppleUSBHubPort			*port;
	bool					returnValue = true;
    
	if (!_ports)
	{
		USBLog(3, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - no _ports - returning false", this);
		return false;
	}
	
	if (_hubHasBeenDisconnected || isInactive() || _hubIsDead)
	{
		USBLog(3, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - hub has been disconnected or is inActive or dead - returning false", this);
		return false;
	}
	
	for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
	{
		portNum = portIndex + 1;
		if (!_ports)
		{
			// I will make this a USBLog(3 after some testing
			USBLog(1, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - the _ports went away - bailing", this);
			returnValue = false;
			break;
		}
        port = _ports[portIndex];
		if (!port)
		{
			USBLog(3, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - no port at index (%d) - returning false", this, (int)portIndex);
			returnValue = false;
			break;
		}
		if ((port->_initThreadActive) || (port->_statusChangedThreadActive))
		{
			USBLog(3, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - port %ld still initing or status changing", this, portNum);
			returnValue = false;
			break;
		}
		kr = GetPortStatus( &portStatus, portNum);
		if ( kr == kIOReturnSuccess )
		{
			USBLog(4, "AppleUSBHub[%p]::HubAreAllPortsDisconnectedOrSuspended - _ports(%p) port(%d) status(%p) change(%p) portDevice(%p)", this, _ports, (int)portNum, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags, port->_portDevice);
			if ( (portStatus.statusFlags & kHubPortConnection) && !(portStatus.statusFlags & kHubPortSuspend) || (portStatus.changeFlags))
			{
				USBLog(7, "AppleUSBHub[%p](0x%lx)::HubAreAllPortsDisconnectedOrSuspended - port %ld enabled and not suspended or there is a change", this, _locationID, portNum);
				returnValue = false;
				break;
			}
		}
		else
		{
			USBLog(1,"AppleUSBHub[%p](0x%lx)::HubAreAllPortsDisconnectedOrSuspended  GetPortStatus for port %ld returned 0x%x", this, _locationID, portNum, kr);
			returnValue = false;
			break;
		}
	}		
	
Exit:

	USBLog(7, "AppleUSBHub[%p](0x%lx)::HubAreAllPortsDisconnectedOrSuspended - returning (%s)", this, _locationID, returnValue ? "true" : "false");
	return returnValue;
}


bool	
AppleUSBHub::IsPortInitThreadActiveForAnyPort()
{
    UInt32					currentPort;
    IOUSBHubPortStatus		portStatus;
	IOReturn				kr;
    AppleUSBHubPort			*port;
	bool					returnValue = false;
    
	if (!_ports)
	{
		USBLog(3, "AppleUSBHub[%p]::IsPortInitThreadActiveForAnyPort - no _ports - returning false", this);
		return false;
	}
	
	for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
	{
        port = _ports ? _ports[currentPort-1] : NULL;
		if (!port)
		{
			USBLog(3, "AppleUSBHub[%p]::IsPortInitThreadActiveForAnyPort - no port object[index: %d]", this, (int)currentPort-1);
			continue;
		}
		
		if ( port->_initThreadActive)
		{
			
			USBLog(4, "AppleUSBHub[%p]::IsPortInitThreadActiveForAnyPort - port(%d) had the devZero lock or initThreadActive(%d)", this, (int)currentPort, port->_initThreadActive);
			returnValue = true;
			break;
		}
	}		
	
	USBLog(6, "AppleUSBHub[%p](0x%lx)::IsPortInitThreadActiveForAnyPort - %s", this, _locationID, returnValue ? "true" : "false");

	return returnValue;
}



#pragma mark �������� Ports ���������
IOReturn 
AppleUSBHub::StartPorts(void)
{
    AppleUSBHubPort			*port;
    int						portIndex, portNum;
	IOReturn				err;
	bool					resumedOneAlready = false;
	bool					workToDo = true;				// don't stop until we have done all we can

    USBLog(5, "AppleUSBHub[%p]::StartPorts - _bus[%p] starting (%d) ports isInactive(%s)", this, _bus, _hubDescriptor.numPorts, isInactive() ? "true" : "false");

	if (!_ports)
	{
		USBError(1, "AppleUSBHub[%p]::StartPorts - no memory for ports!!", this);
		return kIOReturnNoMemory;
	}
	
	if (isInactive())
	{
		USBLog(5, "AppleUSBHub[%p]::StartPorts - we are inactive. Nothing to do.", this);
		return kIOReturnSuccess;
	}
	
    for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
    {
		portNum = portIndex+1;
		
        port = _ports[portIndex];
		if (port)
		{
			port->retain();
			if (port->_portPMState == usbHPPMS_uninitialized)
			{
				USBLog(5, "AppleUSBHub[%p]::StartPorts - _bus[%p] port (%d) uninitialized - calling port->start", this, _bus, portNum);
				port->start();
				port->_portPMState = usbHPPMS_active;
				USBLog(5, "AppleUSBHub[%p]::StartPorts - _bus[%p] port[%p] now in state[%d]", this, _bus, port, port->_portPMState);
			}
			else if (port->_portPMState == usbHPPMS_pm_suspended)
			{
				IOUSBHubPortStatus		portStatus;
				bool					enableEndpoints = false;				// usually this is done when the resume completes
				
				// first let's get the state to see if the resume has already changed (which pobably means that it is disconnected
				USBLog(5, "AppleUSBHub[%p]::StartPorts - port[%p] number (%d) suspended by PM - calling GetPortStatus", this, port, portNum);
				portStatus.statusFlags = 0;
				portStatus.changeFlags = 0;
				err = GetPortStatus(&portStatus, portNum);
				if (err)
				{
					// this happens because we cannot talk to the hub to get the status for the port - usually we (the hub) are no longer on the bus
					// we need to go ahead and re-enable the endpoints before we finish and go to the ON state
					USBLog((err == kIOReturnNoDevice) ? 5 : 1, "AppleUSBHub[%p]::StartPorts - err (%p) from GetPortStatus for port (%d) isInactive(%s) - reeabling endpoints anyway", this, (void*)err, portNum, isInactive() ? "true" : "false");
					enableEndpoints = true;
				}
				else if ((portStatus.changeFlags & kHubPortSuspend) || (portStatus.changeFlags & kHubPortConnection))
				{
					// this is the case where the actual device has been unplugged, but our hub is still on the bus
					USBLog(5, "AppleUSBHub[%p]::StartPorts - port[%p], which is number[%d] has the kHubPortSuspendChange bit or kHubPortConnectionChange already set (probably unplugged) status[%p] change[%p] - won't have to wait! YAY!", this, port, portNum, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags);
					
					// i need to clear the suspend change feature because it cannot get processed again without wreaking havoc
					if (portStatus.changeFlags & kHubPortSuspend)
						ClearPortFeature(kUSBHubPortSuspendChangeFeature, portNum);
					
					enableEndpoints = true;
					
				}
				else
				{
					USBLog(5, "AppleUSBHub[%p]::StartPorts - port[%p] number (%d) suspended by PM statusFlags[%p] - calling SuspendPort(false)", this, port, portNum, (void*)portStatus.statusFlags);
					if (!(portStatus.statusFlags & kHubPortSuspend))
					{
						// I don't think this will ever happen, but it needs to be a USBError for now until I make sure of it
						USBLog(1, "AppleUSBHub[%p]::StartPorts - port %d marked as suspended, but the suspend status bit is not set status[%p] change[%p] _portDevice name[%s]", this, (int)portNum, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags, port->_portDevice ? port->_portDevice->getName() : "no device");
					}
					if (!resumedOneAlready)
					{
						err = port->SuspendPort(false, false);
						resumedOneAlready = true;
					}
					else
						err = kIOReturnSuccess;							// we pretend that we resumed - it will be re-handled in WaitForPortResumes
					
					// the rest of what is needed to wake this port will be done in the HandleSuspendPortChange
					if (err == kIOReturnNotResponding)
					{
						USBLog(5, "AppleUSBHub[%p]::StartPorts - got err kIOReturnNotResponding from SuspendPort- we must be dead! aborting StartPorts", this);
						break;
					}
					if (err != kIOReturnSuccess)
					{
						USBLog(5, "AppleUSBHub[%p]::StartPorts - got err %p from SuspendPort- perhaps we need to deal with this!", this, (void*)err);
					}
					else
					{
						// these will be decremented when the resume notification comes in in WaitForPortResumes
						// we need these even for ports which we didn't actually resume yet. The will get resumed in WaitForPortResumes (one at a time)
						IncrementOutstandingIO();
						IncrementOutstandingResumes();
					}
				}
				if (enableEndpoints)
				{
					IOUSBControllerV3		*v3Bus = NULL;
					IOReturn				err;

					// wait at least 10 ms for port recovery before re-enabling the endpoints on the list
					IOSleep(port->_portResumeRecoveryTime);
					if (_device)
						v3Bus = OSDynamicCast(IOUSBControllerV3, _device->GetBus());
					
					if (v3Bus && port->_portDevice)
					{
						USBLog(5, "AppleUSBHub[%p]::StartPorts - Enabling endpoints for device at address (%d)", this, (int)port->_portDevice->GetAddress());
						err = v3Bus->EnableAddressEndpoints(port->_portDevice->GetAddress(), true);
						if (err)
						{
							USBLog(5, "AppleUSBHub[%p]::StartPorts - EnableAddressEndpoints returned (%p)", this, (void*)err);
						}
					}
					port->_portPMState = usbHPPMS_active;
				}
			}
			else
			{
				USBLog(5, "AppleUSBHub[%p]::StartPorts - port[%p] number (%d) in PMState (%d) _portDevice[%p] - nothing to do", this, port, portNum, port->_portPMState, port->_portDevice);
			}
			port->release();
		}
		else
		{
			USBError(1, "AppleUSBHub[%p]::StartPorts - port (%d) unexpectedly missing", this, portNum);
		}

    }
    return kIOReturnSuccess;
}



IOReturn 
AppleUSBHub::SuspendPorts(void)
{
    AppleUSBHubPort			*port;
    int						currentPort;
	IOReturn				err;
	IOUSBControllerV3		*v3Bus;

    USBLog(5, "AppleUSBHub[%p]::SuspendPorts - (%d) ports", this, _hubDescriptor.numPorts);

    for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
    {
        port = _ports ? _ports[currentPort-1] : NULL;
		if (port)
		{
			port->retain();
			if ((port->_portPMState == usbHPPMS_active) && (port->_portDevice))
			{
				USBLog(5, "AppleUSBHub[%p]::SuspendPorts - suspending port[%p] number (%d) - need to disable device (%p) named(%s)", this, port, currentPort, port->_portDevice, port->_portDevice ? port->_portDevice->getName() : "NULL");
				v3Bus = OSDynamicCast(IOUSBControllerV3, _bus);
				if (v3Bus)
				{
					err = v3Bus->EnableAddressEndpoints(port->_portDevice->GetAddress(), false);
					if (err)
					{
						USBLog(2, "AppleUSBHub[%p]::SuspendPorts - EnableAddressEndpoints returned (%p)", this, (void*)err);
					}
				}
				err = port->SuspendPort(true, false);
				if (err)
				{
					USBLog(1, "AppleUSBHub[%p]::SuspendPorts - err [%p] suspending port (%d)", this, (void*)err, currentPort);
				}
				else
				{
					port->_portPMState = usbHPPMS_pm_suspended;
					USBLog(5, "AppleUSBHub[%p]::SuspendPorts - port[%p] now in state[%d]", this, port, port->_portPMState);
				}
			}
			else
			{
				USBLog(5, "AppleUSBHub[%p]::SuspendPorts - port (%d) in _portPMState (%d) _portDevice(%p) - not calling port->SuspendPort", this, currentPort, port->_portPMState, port->_portDevice);
			}
			port->release();
		}
		else
		{
			USBError(1, "AppleUSBHub[%p]::SuspendPorts - port (%d) unexpectedly missing", this, currentPort);
		}
    }
    return kIOReturnSuccess;
}



IOReturn 
AppleUSBHub::StopPorts(void)
{
    AppleUSBHubPort *		port;
    AppleUSBHubPort **	 	cachedPorts;
    int						currentPort;

    USBLog(5, "AppleUSBHub[%p]::+StopPorts - (%d) ports total", this, _hubDescriptor.numPorts);

	// make sure that we don't receive any more status changes, since the ports will now be gone (5388864)
	if ( _interruptPipe )
	{
		USBLog(5, "AppleUSBHub[%p]::StopPorts - aborting pipe", this);
		_abortExpected = true;
		_interruptPipe->Abort();
		USBLog(5, "AppleUSBHub[%p]::StopPorts - done aborting pipe", this);
	}

    if( _ports)
    {
        cachedPorts = _ports;
        _ports = NULL;

        for (currentPort = 0; currentPort < _hubDescriptor.numPorts; currentPort++)
        {
            port = cachedPorts[currentPort];
            if (port)
            {
                cachedPorts[currentPort] = NULL;
				USBLog(5, "AppleUSBHub[%p]::StopPorts - calling stop on port (%p)", this, port);
                port->stop();
				USBLog(5, "AppleUSBHub[%p]::StopPorts - calling release on port (%p)", this, port);
                port->release();
            }
        }
        IOFree(cachedPorts, sizeof(AppleUSBHubPort *) * _hubDescriptor.numPorts);
    }

	if (_extraPower && _parentHubDevice)
	{
		USBLog(2, "AppleUSBHub[%p]::StopPorts - releasing (%d) extra power", this, (int)_extraPower);
		_parentHubDevice->ReturnExtraPower(_extraPower);
	}
	
    return kIOReturnSuccess;
}


bool 
AppleUSBHub::HubStatusChanged(void)
{
    IOReturn	err = kIOReturnSuccess;

	USBLog(6,"+AppleUSBHub[%p]::HubStatusChanged ", this);
	IncrementOutstandingIO();
    do
    {
        if ((err = GetHubStatus(&_hubStatus)))
        {
            FatalError(err, "get status (first in hub status change)");
            break;
        }
        _hubStatus.statusFlags = USBToHostWord(_hubStatus.statusFlags);
        _hubStatus.changeFlags = USBToHostWord(_hubStatus.changeFlags);

        USBLog(3,"AppleUSBHub[%p]::HubStatusChanged - _isRootHub(%s) hub status = %x/%x", this, _isRootHub ? "true" : "false", _hubStatus.statusFlags, _hubStatus.changeFlags);

        if (_hubStatus.changeFlags & kHubLocalPowerStatusChange)
        {
            USBLog(3, "AppleUSBHub[%p]: Hub Local Power Status Change detected", this);
            if ((err = ClearHubFeature(kUSBHubLocalPowerChangeFeature)))
            {
                FatalError(err, "clear hub power status feature");
                break;
            }
            if ((err = GetHubStatus(&_hubStatus)))
            {
                FatalError(err, "get status (second in hub status change)");
                break;
            }
            
			_hubStatus.statusFlags = USBToHostWord(_hubStatus.statusFlags);
            _hubStatus.changeFlags = USBToHostWord(_hubStatus.changeFlags);
			
			USBLog(3,"AppleUSBHub[%p]: hub status after clearing LocalPowerChange = %x/%x", this, _hubStatus.statusFlags, _hubStatus.changeFlags);
			// Need to check whether we successfully cleared the change
        }

        if (_hubStatus.changeFlags & kHubOverCurrentIndicatorChange)
        {
            USBLog(3, "AppleUSBHub[%p]: Hub OverCurrent Indicator Change detected", this);
            if ((err =
                 ClearHubFeature(kUSBHubOverCurrentChangeFeature)))
            {
                FatalError(err, "clear hub over current feature");
                break;
            }
            if ((err = GetHubStatus(&_hubStatus)))
            {
                FatalError(err, "get status (second in hub status change)");
                break;
            }
            _hubStatus.statusFlags = USBToHostWord(_hubStatus.statusFlags);
            _hubStatus.changeFlags = USBToHostWord(_hubStatus.changeFlags);
			USBLog(3,"AppleUSBHub[%p]: hub status after clearing HubOvercurrent = %x/%x", this, _hubStatus.statusFlags, _hubStatus.changeFlags);
        }

        // See if we have the kResetOnPowerStatusChange errata. This means that upon getting a hub status change, we should do
        // a device reset
        //
        OSBoolean * boolObj = OSDynamicCast( OSBoolean, _device->getProperty("kResetOnPowerStatusChange") );
        if ( boolObj && boolObj->isTrue() )
        {
            // Set an error so that we cause our port to be reset. The overcurrent and power status changes might disable the ports downstream
            // so we need a reset to recover. Do this ONLY if the change flag indicated that the status was a change to ON.
            //
			if ( _hubStatus.statusFlags & ( kUSBHubLocalPowerChangeFeature || kHubOverCurrentIndicatorChange ) )
			{
				USBLog(3,"AppleUSBHub[%p]::HubStatusChanged -  change to ON (0x%x)", this, _hubStatus.statusFlags);
				err = kIOReturnBusy;
			}
        }

    } while(false);

    if ( err != kIOReturnSuccess )
	{
        // If we get an error, then we better reset our hub
        //
		USBLog(1, "AppleUSBHub[%p]::HubStatusChanged - err (%p) - reseting my port", this, (void*)err);
        retain();
        ResetMyPort();
        release();
        
    }

	DecrementOutstandingIO();
	USBLog(6,"-AppleUSBHub[%p]::HubStatusChanged returning %s", this, err == kIOReturnSuccess ? "true" : "false");
	return (err == kIOReturnSuccess);
}

UInt32 
AppleUSBHub::GetHubErrataBits()
{
      UInt16		vendID, deviceID, revisionID;
      ErrataListEntry	*entryPtr;
      UInt32		i, errata = 0;

      // get this chips vendID, deviceID, revisionID
      vendID = _device->GetVendorID();
      deviceID = _device->GetProductID();
      revisionID = _device->GetDeviceRelease();

      for(i=0, entryPtr = errataList; i < errataListLength; i++, entryPtr++)
      {
          if (vendID == entryPtr->vendID
              && deviceID == entryPtr->deviceID
              && revisionID >= entryPtr->revisionLo
              && revisionID <= entryPtr->revisionHi)
          {
              errata |= entryPtr->errata;  // we match, add this errata to our list
          }
      }
      return(errata);
}



void 
AppleUSBHub::FatalError(IOReturn err, const char *str)
{
    USBError(1, "AppleUSBHub[%p]::FatalError 0x%x: %s", this, err, str);
}



IOReturn 
AppleUSBHub::GetHubDescriptor(IOUSBHubDescriptor *desc)
{
    IOReturn	err = kIOReturnSuccess;
    IOUSBDevRequest	request;

    if (!desc) return (kIOReturnBadArgument);

    request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBDevice);
    request.bRequest = kUSBRqGetDescriptor;
    request.wValue = (kUSBHubDescriptorType << 8) + 0;	// Descriptor type goes in high byte, index in low
    request.wIndex = 0;
    request.wLength = sizeof(IOUSBHubDescriptor);
    request.pData = desc;

    err = DoDeviceRequest(&request);

    if (err)
    {
        /*
         * Is this a bogus hub?  Some hubs require 0 for the descriptor type
         * to get their device descriptor.  This is a bug, but it's actually
         * spec'd out in the USB 1.1 docs.
         */
        USBLog(5,"AppleUSBHub[%p]: GetHubDescriptor w/ type = %X returned error: 0x%x", this, kUSBHubDescriptorType, err);
        request.wValue = 0;
        request.wLength = sizeof(IOUSBHubDescriptor);
        err = DoDeviceRequest(&request);
    }

    if (err)
    {
        USBLog(3, "AppleUSBHub [%p] GetHubDescriptor error = 0x%x", this, err);
    }        

    return(err);
}



IOReturn 
AppleUSBHub::GetHubStatus(IOUSBHubStatus *status)
{
    IOReturn		err = kIOReturnSuccess;
    IOUSBDevRequest	request;

    request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBDevice);
    request.bRequest = kUSBRqGetStatus;
    request.wValue = 0;
    request.wIndex = 0;
    request.wLength = sizeof(IOUSBHubStatus);
    request.pData = status;

    err = DoDeviceRequest(&request);

    if (err)
    {
        USBLog(3, "AppleUSBHub [%p] GetHubStatus error = 0x%x", this, err);
    }        

    return(err);
}



IOReturn 
AppleUSBHub::GetPortState(UInt8 *state, UInt16 port)
{
    IOReturn		err = kIOReturnSuccess;
    IOUSBDevRequest	request;

    request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBOther);
    request.bRequest = kUSBRqGetState;
    request.wValue = 0;
    request.wIndex = port;
    request.wLength = sizeof(*state);
    request.pData = state;

    err = DoDeviceRequest(&request);

    if (err)
    {
        USBLog(3, "AppleUSBHub [%p] GetPortState error = 0x%x", this, err);
    }        

    return(err);
}



IOReturn 
AppleUSBHub::ClearHubFeature(UInt16 feature)
{
    IOReturn		err = kIOReturnSuccess;
    IOUSBDevRequest	request;

    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBDevice);
    request.bRequest = kUSBRqClearFeature;
    request.wValue = feature;
    request.wIndex = 0;
    request.wLength = 0;
    request.pData = NULL;

    err = DoDeviceRequest(&request);

    if (err)
    {
        USBLog(3, "AppleUSBHub [%p] ClearHubFeature error = 0x%x", this, err);
    }        

    return(err);
}



IOReturn 
AppleUSBHub::GetPortStatus(IOUSBHubPortStatus *status, UInt16 port)
{
    IOReturn			err = kIOReturnSuccess;
    IOUSBDevRequest		request;
	int					i = 0;

    request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBOther);
    request.bRequest = kUSBRqGetStatus;
    request.wValue = 0;
    request.wIndex = port;
    request.wLength = sizeof(IOUSBHubPortStatus);
    request.pData = status;
	request.wLenDone = 0;

	USBLog(7, "AppleUSBHub[%p]::GetPortStatus - issuing DeviceRequest", this);
    err = DoDeviceRequest(&request);
	
	while ((i++ < 30) && !err && (request.wLenDone != sizeof(IOUSBHubPortStatus)))
	{
		USBLog(2, "AppleUSBHub[%p]::GetPortStatus - request came back with only %d bytes - retrying", this, (int)request.wLenDone);
		err = DoDeviceRequest(&request);
	}

	if (!err && (request.wLenDone != sizeof(IOUSBHubPortStatus)))
	{
		USBLog(2, "AppleUSBHub[%p]::GetPortStatus - request never returned bytes in %d tries - returning kIOReturnUnderrun", this, i);
		err = kIOReturnUnderrun;
	}

    if (err)
    {
        USBLog(3, "AppleUSBHub[%p]::GetPortStatus, error (%x) returned from DoDeviceRequest", this, err);
    }

    if ( err == kIOReturnSuccess)
    {
		// Get things the right way round.
		status->statusFlags = USBToHostWord(status->statusFlags);
		status->changeFlags = USBToHostWord(status->changeFlags);
		
        USBLog( 7, "AppleUSBHub[%p]::GetPortStatus for port %d, status: 0x%8x, change: 0x%8x - returning kIOReturnSuccess", this, port, status->statusFlags, status->changeFlags);
    }
    
    return(err);
}



IOReturn 
AppleUSBHub::SetPortFeature(UInt16 feature, UInt16 port)
{
    IOReturn		err = kIOReturnSuccess;
    IOUSBDevRequest	request;

    USBLog(5, "AppleUSBHub[%p]::SetPortFeature port/feature (%x) - setting", this, (port << 16) | feature);

    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBOther);
    request.bRequest = kUSBRqSetFeature;
    request.wValue = feature;
    request.wIndex = port;
    request.wLength = 0;
    request.pData = NULL;

    err = DoDeviceRequest(&request);

    if (err && (err != kIOUSBDeviceNotHighSpeed))
    {
        USBLog(1, "AppleUSBHub[%p]::SetPortFeature (%d) to port %d got error (%x) from DoDeviceRequest", this, feature, port, err);
    }

    return(err);
}



IOReturn 
AppleUSBHub::ClearPortFeature(UInt16 feature, UInt16 port)
{
    IOReturn			err = kIOReturnSuccess;
    IOUSBDevRequest		request;
	
    USBLog(5, "AppleUSBHub[%p]::ClearPortFeature port/feature (%x) - clearing", this, (port << 16) | feature);

    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBOther);
    request.bRequest = kUSBRqClearFeature;
    request.wValue = feature;
    request.wIndex = port;
    request.wLength = 0;
    request.pData = NULL;

    err = DoDeviceRequest(&request);

    if (err)
    {
        USBLog(1, "AppleUSBHub[%p]::ClearPortFeature got error (%x) to DoDeviceRequest", this, err);
    }

    return err;
}



IOReturn 
AppleUSBHub::DoPortAction(UInt32 type, UInt32 portNumber, UInt32 options )
{
    AppleUSBHubPort 		*port;
    IOReturn				err = kIOReturnSuccess;
	bool					tryWakingUp = true;
	UInt32					portIndex = portNumber - 1;
	
	
	if (isInactive() || _hubIsDead)
	{
		USBLog(3, "AppleUSBHub[%p]::DoPortAction - isInactive(%s) _hubIsDead(%s) - returning kIOReturnNotResponding", this, isInactive() ? "true" : "false", _hubIsDead ? "true" : "false");
		err =  kIOReturnNotResponding;
	}
	else
	{
		if (_myPowerState < kIOUSBHubPowerStateOn)
		{
			if (_powerStateChangingTo != kIOUSBHubPowerStateStable)
			{
				// we are in the process of doing a power state change
				// that is, we have had a call to powerStateWillChangeTo and haven't yet gotten to powerStateDone
				// unless the new state is ON, then we will not be able to get to that state
				if ((unsigned long)_powerStateChangingTo != kIOUSBHubPowerStateOn)
				{
					USBLog(5,"AppleUSBHub[%p]::DoPortAction - we are already in the process of changing to state (%d) - unable to wake up - returning not responding", this, (int)_powerStateChangingTo);
					tryWakingUp = false;
				}
			}
			else if (_myPowerState < kIOUSBHubPowerStateLowPower)
			{
				// we are not actively changing state, but we are apparently already in a state which we cannot wake up from this way
				USBLog(5,"AppleUSBHub[%p]::DoPortAction - not while we are asleep - _myPowerState(%d)  - returning not responding", this, (int)_myPowerState);
				tryWakingUp = false;
			}
			if (!tryWakingUp)
			{
				USBLog(2, "AppleUSBHub[%p]::DoPortAction - _myPowerState(%d) _powerStateChangingTo(%d) - returning kIOReturnNotResponding", this, (int)_myPowerState, (int)_powerStateChangingTo);
				err = kIOReturnNotResponding;
				port = _ports ? _ports[portIndex] : NULL;
				if (port && port->_portDevice)
				{
					USBLog(5,"AppleUSBHub[%p]::DoPortAction - _portDevice name is %s", this, port->_portDevice->getName());
				}
			}
		}
	}

	if (err == kIOReturnSuccess)
	{
		SInt32			retries = 200;
		USBLog(5,"AppleUSBHub[%p]::DoPortAction - calling RaisePowerState", this);
		RaisePowerState();
		
		// if we are not in the On state, or we are in the process of swithing to LowPower, the we need to get to the On state..
		while (((_myPowerState < kIOUSBHubPowerStateOn) || (_powerStateChangingTo == kIOUSBHubPowerStateLowPower)) && retries--)
		{
			USBLog(5,"AppleUSBHub[%p]::DoPortAction - waiting for power state to go up (onThread is %s)", this, _workLoop->onThread() ? "true" : "false");
			IOSleep(10);
		}
		if (retries <= 0)
		{
			USBLog(3,"AppleUSBHub[%p]::DoPortAction - waited 2 seconds - _myPowerState(%d) _powerStateChangingTo(%d)", this, (int)_myPowerState, (int)_powerStateChangingTo);
		}
		if (_myPowerState < kIOUSBHubPowerStateOn)
		{
			USBLog(3,"AppleUSBHub[%p]::DoPortAction - could not get power state up - returning kIOReturnNotResponding", this);
			err =  kIOReturnNotResponding;
		}
		else if ( _portSuspended && _device )
		{
			// If we are suspended, we need to first wake up
			USBLog(5,"AppleUSBHub[%p]::DoPortAction(%s) for port (%ld), unsuspending port", this, HubMessageToString(type), portNumber);
			err = _device->SuspendDevice(false);
			if ( err == kIOReturnSuccess )
			{
				IOSleep(_hubResumeRecoveryTime);						// give ourselves time to recover
			}
		}	
		if ( err != kIOReturnSuccess )
		{
			LowerPowerState();
		}
	}

    if ((_ports == NULL ) && (err == kIOReturnSuccess))
	{
		USBLog(1,"AppleUSBHub[%p]::DoPortAction  _ports is NULL! - returning kIOReturnNoDevice", this);
		LowerPowerState();								// we raised this above because there had been no error
		err = kIOReturnNoDevice;
	}
	
	if (err != kIOReturnSuccess)
	{
		USBLog(5, "AppleUSBHub[%p]::DoPortAction - unable to find success in the setup - err(%p)", this, (void*)err);
		if ((type == kIOUSBMessageHubResumePort) || (type == kIOUSBMessageHubSuspendPort))
		{
			IOUSBDevice *cachedDevice;					// in case _portDevice goes away while we are messaging
			USBLog(2, "AppleUSBHub[%p]::DoPortAction - err(%p) message was %s - need to send a message", this, (void*)err, HubMessageToString(type));
			port = _ports ? _ports[portIndex] : NULL;
			cachedDevice = port ? port->_portDevice : NULL;
			if (cachedDevice)
			{
				cachedDevice->retain();
				cachedDevice->message(kIOUSBMessagePortWasNotSuspended, cachedDevice, NULL);
				cachedDevice->release();
			}
			else
			{
				USBLog(2, "AppleUSBHub[%p]::DoPortAction - err(%p) - message was %s - no portDevice to send message to", this, (void*)err, HubMessageToString(type));
			}
		}
		return err;
	}
			
	USBLog(5,"+AppleUSBHub[%p]::DoPortAction(%s) for port (%ld), options (0x%lx) _myPowerState(%d), getting _doPortActionLock", this, HubMessageToString(type), portNumber, options, (int)_myPowerState);
	IOLockLock(_doPortActionLock);
	
	// no longer do this as it affects the interrupt read pipe for all ports
	// USBLog(5,"AppleUSBHub[%p]::DoPortAction - got _doPortActionLock, calling IncrementOutstandingIO", this);
	// IncrementOutstandingIO();

	port = _ports ? _ports[portIndex] : NULL;
    if (port)
    {
		// Keep a reference while we work with the port
		port->retain();		
		USBLog(5,"AppleUSBHub[%p]::DoPortAction(%s) for port (%ld), options (0x%lx) _portPMState (%d)", this, HubMessageToString(type), portNumber, options, port->_portPMState);
		
        switch ( type )
        {
            case kIOUSBMessageHubSuspendPort:
				// this completes essentially synchronously
				err = port->SuspendPort( true, true );
				USBLog(5, "AppleUSBHub[%p]::DoPortAction - port[%p] now in state[%d]", this, port, port->_portPMState);
				break;

           case kIOUSBMessageHubResumePort:
				EnsureUsability();
                err = port->SuspendPort( false, true );
				USBLog(5, "AppleUSBHub[%p]::DoPortAction - port[%p] now in state[%d]", this, port, port->_portPMState);
				if (!err && (_myPowerState == kIOUSBHubPowerStateLowPower))
				{
					// this only happens with root hubs which are the only hubs we can communicate with while they are in low power mode
					// however, since there is no outstanding interrupt read, we need to check the port status ourselves
					if (!_isRootHub)
					{
						USBError(1, "AppleUSBHub[%p]::DoPortAction - resuming port, but we are not a root hub - shouldn't be here", this);
					}
					else
					{
						int						retries = 5000;
						IOUSBHubPortStatus		portStatus;
						// since we won't have an interrupt read to complete the resume, we need to wait for it ourselves - it takes at least 20 ms
						// note - this is something that happens outside of the gate most of the time
						USBLog(5, "AppleUSBHub[%p]::DoPortAction - sleeping 20ms for the resume - onThread(%s)", this, _workLoop->onThread() ? "true" : "false");
						IOSleep(20);
						err = GetPortStatus(&portStatus, portNumber);
						while (!err && retries-- > 0)
						{
							USBLog(5, "AppleUSBHub[%p]::DoPortAction - got statusFlags(%p) changeFlags(%p)", this, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags);
							if (portStatus.changeFlags & kHubPortSuspend)
							{
								ClearPortFeature(kUSBHubPortSuspendChangeFeature, portNumber);
								USBLog(3, "AppleUSBHub[%p]::DoPortAction - port[%p], which is number[%d] has the suspend change bit set- good - statusFlags[%p] and changeFlags[%p]", this, port, (int)portNumber, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags);
								port->_portPMState = usbHPPMS_active;
								port->_resumePending = false;
								if ( port->_portDevice)
								{
									IOUSBDevice *cachedDevice = port->_portDevice;			// in case _portDevice goes away while we are messaging
									
									if (cachedDevice)
									{
										cachedDevice->retain();
										cachedDevice->message(kIOUSBMessagePortHasBeenResumed, cachedDevice, NULL);
										cachedDevice->release();
									}
								}
								break;
							}
							USBLog(3, "AppleUSBHub[%p]::DoPortAction - port still not resumed - sleeping 1ms before trying again - retries left (%d)", this, retries);
							IOSleep(1);
							err = GetPortStatus(&portStatus, portNumber);
						}
					}
				}
               break;

            case kIOUSBMessageHubReEnumeratePort:
                err = port->ReEnumeratePort(options);
                break;
				
            case kIOUSBMessageHubResetPort:
                err = port->ResetPort();
                break;
				
            case kIOUSBMessageHubPortClearTT:
                // ClearTT is only supported on HS Hubs
                //
                if ( _hsHub )
                    err = port->ClearTT(_multiTTs, options);
                else
                    err = kIOReturnUnsupported;
                break;
			
			case kIOUSBMessageHubSetPortRecoveryTime:
				USBLog(5, "AppleUSBHub[%p]::DoPortAction - port %ld, setting _portResumeRecoveryTime to %ld", this, portNumber, options);
				err = port->_portResumeRecoveryTime = options;
                break;
				
		}
		
		// and now, release our reference
		port->release();
    }

    // since we stopped doing the Increment above, we need to not do this either
	// USBLog(5,"AppleUSBHub[%p]::DoPortAction - calling DecrementOutstandingIO", this);
	// DecrementOutstandingIO();

	// since holding this lock can cause us to delay a close call, we will do an Increment/Decrement to cause a check here
	// we will Increment before we unlock the lock and Decrement afterwards
	IncrementOutstandingIO();
	IOLockUnlock(_doPortActionLock);
	DecrementOutstandingIO();
	
	USBLog(5,"AppleUSBHub[%p]::DoPortAction - calling LowerPowerState", this);
	LowerPowerState();

	// since a port action could have caused all the ports to now be suspended, spawn a thread to see
	// we do this because the outstanding io might be 1, because we have an outstanding read which will 
	// not complete in this case
	if (!isInactive() && !_hubIsDead && !_checkPortsThreadActive)
	{
		_checkPortsThreadActive = true;
		retain();											// in case we get terminated while the thread is still waiting to be scheduled
		thread_call_enter(_checkForActivePortsThread);
	}

	USBLog(5,"-AppleUSBHub[%p]::DoPortAction(%s) for port (%ld), returning 0x%x", this, HubMessageToString(type), portNumber, err);
	
    return err;
}

void 
AppleUSBHub::InterruptReadHandlerEntry(OSObject *target, void *param, IOReturn status, UInt32 bufferSizeRemaining)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);

    if (!me)
        return;
    
	USBLog(5, "AppleUSBHub[%p]::InterruptReadHandlerEntry", me);
    me->InterruptReadHandler(status, bufferSizeRemaining);
    me->DecrementOutstandingIO();
}



void 
AppleUSBHub::InterruptReadHandler(IOReturn status, UInt32 bufferSizeRemaining)
{
    bool			queueAnother = TRUE;
    IOReturn		err = kIOReturnSuccess;

	switch (status)
	{
		case kIOReturnOverrun:
			USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler kIOReturnOverrun error", this);
			// This is an interesting error, as we have the data that we wanted and more...  We will use this
			// data but first we need to clear the stall and reset the data toggle on the device.  We then just 
			// fall through to the kIOReturnSuccess case.
			
			if (!isInactive())
			{
				//
				// First, clear the halted bit in the controller
				//
				if ( _interruptPipe )
				{
					_interruptPipe->ClearStall();
					
					// And call the device to reset the endpoint as well
					//
					IncrementOutstandingIO();
					thread_call_enter(_clearFeatureEndpointHaltThread);
				}
			}
				
				// Fall through to process the data.
				
			case kIOReturnSuccess:
				
				_retryCount = kHubDriverRetryCount;
				
				if (!_ports)
				{
					USBLog(1, "AppleUSBHub[%p]::InterruptReadHandler - avoiding NULL _ports - unlike before!!", this);
				}
			
				// Handle the data
				//
				if ( !_hubIsDead  && (_ports != NULL))
				{
					IncrementOutstandingIO();
					thread_call_enter(_workThread);
				}
				
				// Note that the workThread will requeue the interrupt, so we don't
				// need to do it again
				//
				queueAnother = FALSE;
				
				break;
				
			case kIOReturnNotResponding:
				// If our device has been disconnected or we're already processing a
				// terminate message, just go ahead and close the device (i.e. don't
				// queue another read.  Otherwise, go check to see if the device is
				// around or not. 
				//
				USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler error kIOReturnNotResponding", this);
				
				if ( _hubHasBeenDisconnected || isInactive() )
				{
					queueAnother = false;
				}
				else
				{
					USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler Checking to see if hub is still connected", this);
					
					CallCheckForDeadHub();
					
					// Note that since we don't do retries on the Hub, if we get a kIOReturnNotResponding error
					// we will either determine that the hub is disconnected or we will reset the hub.  In either
					// case, we will not need to requeue the interrupt, so we don't need to clear the stall.
					
					queueAnother = false;
					
				}
				
				break;
				
			case kIOReturnAborted:
				// This generally means that we are done, because we were unplugged, but not always
				//
				if (isInactive() || _hubIsDead || _abortExpected )
				{
					USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler error kIOReturnAborted (expected)", this);
					queueAnother = false;
				}
				else
				{
					USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler error kIOReturnAborted. Try again.", this);
				}
				break;
				
			case kIOReturnUnderrun:
			case kIOUSBPipeStalled:
			case kIOUSBLinkErr:
			case kIOUSBNotSent2Err:
			case kIOUSBNotSent1Err:
			case kIOUSBBufferUnderrunErr:
			case kIOUSBBufferOverrunErr:
			case kIOUSBWrongPIDErr:
			case kIOUSBPIDCheckErr:
			case kIOUSBDataToggleErr:
			case kIOUSBBitstufErr:
			case kIOUSBCRCErr:
				// These errors will halt the endpoint, so before we requeue the interrupt read, we have
				// to clear the stall at the controller and at the device.
				//
				USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler OHCI error (0x%x) reading interrupt pipe", this, status);
				
				// 01-28-02 JRH If we are inactive, then we can ignore this
				//
				if (!isInactive())
				{
					// First, clear the halted bit in the controller
					//
					if ( _interruptPipe )
					{
						_interruptPipe->ClearStall();
						
						// And call the device to reset the endpoint as well
						//
						_needInterruptRead = true;
						IncrementOutstandingIO();
						thread_call_enter(_clearFeatureEndpointHaltThread);
					}
				}
					
					queueAnother = false;
				break;
				
			case kIOUSBHighSpeedSplitError:
				// If our device has been disconnected or we're already processing a
				// terminate message, just go ahead and close the device (i.e. don't
				// queue another read.  Otherwise, go check to see if the device is
				// around or not. 
				//
				USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler error kIOUSBHighSpeedSplitError", this);
				
				if ( _hubHasBeenDisconnected || isInactive() )
				{
					queueAnother = false;
				}
				else
				{
					USBLog(3, "AppleUSBHub[%p]::InterruptReadHandler Checking to see if hub is still connected", this);
					
					CallCheckForDeadHub();
					
					// Note that since we don't do retries on the Hub, if we get a kIOReturnNotResponding error
					// we will either determine that the hub is disconnected or we will reset the hub.  In either
					// case, we will not need to requeue the interrupt, so we don't need to clear the stall.
					
					queueAnother = false;
					
				}
				
				break;
				
			default:
				USBLog(3,"AppleUSBHub[%p]::InterruptReadHandler error 0x%x reading interrupt pipe", this, status);
				if (isInactive() || ((_powerStateChangingTo != kIOUSBHubPowerStateStable) && (_powerStateChangingTo < kIOUSBHubPowerStateLowPower)) )
					queueAnother = false;
				else
				{
					// Clear the halted bit in the controller
					//
					if ( _interruptPipe )
						_interruptPipe->ClearStall();
				}
					break;
	}
    if ( queueAnother )
    {
        // RearmInterruptRead will close the device if it fails, so we don't need to do it here
        //
        _needInterruptRead = true;
    }
}



void
AppleUSBHub::ResetPortZeroEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);
    
    if (!me)
        return;
        
    me->ResetPortZero();
    me->DecrementOutstandingIO();
}



void
AppleUSBHub::ResetPortZero()
{
    AppleUSBHubPort 	*port;
    UInt32		currentPort;

    // Find out which port we have to reset
    //
    if( _ports) 
        for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
        {
            port = _ports ? _ports[currentPort-1] : NULL;
            if (port) 
            {
                Boolean	locked;
				port->retain();
                locked = port->GetDevZeroLock();
                if ( locked )
                {
                    // If the timeout flag for this port is already set, AND the timestamp is the same as when we
                    // first detected the lock, then is time to release the devZero lock
                    if ( ((_timeoutFlag & (1 << (currentPort-1))) != 0) && (_portTimeStamp[currentPort-1] == port->GetPortTimeStamp()) )
                    {
                        USBLog(1, "AppleUSBHub[%p]::ResetPortZero: - port %ld - Releasing devZero lock", this, currentPort);
                        _timeoutFlag &= ~( 1<<(currentPort-1));
                        port->ReleaseDevZeroLock();
                    }
                }
				port->release();
            }
        }
}

//
// ProcessStatusChanged
// This method will run on one of the shared kernel threads. It is called when an Async read
// on the interrupt pipe returns some data. It needs to issue another read when it is done
// processing, and then just return
//
void 
AppleUSBHub::ProcessStatusChangedEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);
    
    if (!me)
        return;
        
    me->ProcessStatusChanged();
    me->DecrementOutstandingIO();
}



void 
AppleUSBHub::ProcessStatusChanged()
{
    const UInt8	*		statusChangedBitmapPtr = 0;
    int					portMask;
    int					portByte;
    int					portIndex, portNum;
    AppleUSBHubPort 	*port;
    bool				portSuccess = false;
    bool				hubStatusSuccess = true;

    if (isInactive() || !_buffer || !_ports)
        return;

    portMask = 2;
    portByte = 0;
    statusChangedBitmapPtr = (const UInt8*)_buffer->getBytesNoCopy();
    if (statusChangedBitmapPtr == NULL)
    {
        USBError(1, "AppleUSBHub[%p]::ProcessStatusChanged: No interrupt pipe buffer!", this);
    }
    else
    {
        if ( statusChangedBitmapPtr[0] == 0xff)
        {
            USBLog(5,"AppleUSBHub[%p]::ProcessStatusChanged found (FF) in statusChangedBitmap", this);
        }
        else
        {
			// We need to indicate that we want a read after all is said and done
			_needInterruptRead = true;
			
           if ((statusChangedBitmapPtr[0] & 1) != 0)
            {
				hubStatusSuccess = HubStatusChanged();
            }

            if ( hubStatusSuccess )
            {
				// check these again to make sure that we didn't go away while in HubStatusChanged
				if (isInactive() || !_buffer || !_ports)
				{
					USBLog(1,"AppleUSBHub[%p]::ProcessStatusChanged - in inner loop - we seem to have gone away. bailing", this);
					return;
				}

                USBLog(5,"AppleUSBHub[%p]::ProcessStatusChanged - calling IncrementOutstandingIO", this);
				IncrementOutstandingIO();				// once for the master count of this loop
                USBLog(5,"AppleUSBHub[%p]::ProcessStatusChanged found (0x%8.8x) in statusChangedBitmap", this, statusChangedBitmapPtr[0]);
                for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
                {
					portNum = portIndex + 1;
                    if ((statusChangedBitmapPtr[portByte] & portMask) != 0)
                    {
                        port = _ports[portIndex];
						if ( port )
						{
							USBLog(5,"AppleUSBHub[%p]::ProcessStatusChanged port number %d, calling IncrementOutstandingIO and port->StatusChanged", this, portNum);
							// note that the StatusChanged call below will happen on another thread
							// which means that we will end up Rearming the interrupt read before it is actually done
							IncrementOutstandingIO();					// once for each port which changes
							RaisePowerState();							// same for this...
							portSuccess = port->StatusChanged();
							if (! portSuccess )
							{
								USBLog(1,"AppleUSBHub[%p]::ProcessStatusChanged port->StatusChanged() returned false", this);
							}
						}
                    }

                    portMask <<= 1;
                    if (portMask > 0x80)
                    {
                        portMask = 1;
                        portByte++;
                    }
                }
                USBLog(5,"AppleUSBHub[%p]::ProcessStatusChanged - calling DecrementOutstandingIO", this);
                DecrementOutstandingIO();					// this is the master - will rearm the read if necessary
            }
			else
			{
                USBLog(1,"AppleUSBHub[%p]::ProcessStatusChanged - hubStatusSuccess(FALSE) - this is weird", this);
			}
        }
    }
}


IOReturn
AppleUSBHub::RearmInterruptRead()
{
    IOReturn		err = kIOReturnSuccess;
    IOUSBCompletion	comp;

	USBLog(5,"+AppleUSBHub[%p]::RearmInterruptRead", this);
	
	// only read if we are in ON state
	if (_myPowerState < kIOUSBHubPowerStateOn)
	{
		USBLog(1,"AppleUSBHub[%p]::RearmInterruptRead - unexpected power state (%d)", this, (int)_myPowerState);
		_needInterruptRead = true;
		return err;
	}	
	
	USBLog(5,"+AppleUSBHub[%p]::RearmInterruptRead - calling IncrementOutstandingIO", this);
    IncrementOutstandingIO();			// make sure we don't get closed before the callback

	if (!_ports)
	{
		USBLog(1, "AppleUSBHub[%p]::RearmInterruptRead - avoiding NULL _ports - unlike before!!", this);
	}
	
    if ( isInactive() || (_buffer == NULL) || ( _interruptPipe == NULL ) || (_ports == NULL))
	{
		DecrementOutstandingIO();
        return err;
	}
	
    comp.target = this;
    comp.action = (IOUSBCompletionAction) InterruptReadHandlerEntry;
    comp.parameter = NULL;
    _buffer->setLength(_readBytes);

    if ((err = _interruptPipe->Read(_buffer, &comp)))
    {
        USBLog(1,"AppleUSBHub[%p]::RearmInterruptRead error %x reading interrupt pipe", this, err);
        DecrementOutstandingIO();
    }
    
	USBLog(5,"-AppleUSBHub[%p]::RearmInterruptRead", this);
    return err;
}



void 
AppleUSBHub::PrintHubDescriptor(IOUSBHubDescriptor *desc)
{
    int i = 0;
    const char *characteristics[] =
        { "ppsw", "nosw", "comp", "ppoc", "nooc", 0 };


    if (desc->length == 0) return;

    IOLog("hub descriptor: (%d bytes)\n", desc->length);
    IOLog("\thubType = %d\n", desc->hubType);
    IOLog("\tnumPorts = %d\n", desc->numPorts);
    IOLog("\tcharacteristics = %x ( ",
                                USBToHostWord(desc->characteristics));
    do
    {
        if (USBToHostWord(desc->characteristics) & (1 << i))
            IOLog("%s ", characteristics[i]);
    } while (characteristics[++i]);
    IOLog(")\n");
    IOLog("\tpowerOnToGood = %d ms\n", desc->powerOnToGood * 2);
    IOLog("\thubCurrent = %d\n", desc->hubCurrent);
    IOLog("\tremovablePortFlags = %lx %lx\n", (UInt32)&desc->removablePortFlags[1], (UInt32)&desc->removablePortFlags[0]);
    IOLog("\tpwrCtlPortFlags    = %lx %lx\n", (UInt32)&desc->pwrCtlPortFlags[1], (UInt32)&desc->removablePortFlags[0]);
}



IOReturn 
AppleUSBHub::DoDeviceRequest(IOUSBDevRequest *request)
{
    IOReturn err;
    
	USBLog(5, "AppleUSBHub[%p]::DoDeviceRequest - _device[%p] _powerStateChangingTo[%d] _portSuspended[%s]", this, _device, (int)_powerStateChangingTo, _portSuspended ? "true" : "false");
    // Paranoia:  if we don't have a device ('cause it was stop'ped), then don't send
    // the request.
    //
    if ( !_device || _device->isInactive() || !_device->isOpen(this))
	{
		USBLog(1, "AppleUSBHub[%p]::DoDeviceRequest - _device(%p) isInactive(%s) isOpen(%s)  - returning kIOReturnNoDevice", this, _device, (_device && !_device->isInactive()) ? "false" : "true", (_device && _device->isOpen(this)) ? "true" : "false");
        err = kIOReturnNoDevice;
	}
	else
	{
		// If we are suspended, we need to first wake up
		if ( _portSuspended )
		{
			USBLog(5,"+AppleUSBHub[%p]::DoDeviceRequest, unsuspending port", this);
			err = _device->SuspendDevice(false);
			IOSleep(_hubResumeRecoveryTime);		// give ourselves time to recover if we are suspended
			if ( err != kIOReturnSuccess )
				return err;
		}		

        err = _device->DeviceRequest(request, 5000, 0);
	}
        
	USBLog(5, "AppleUSBHub[%p]::DoDeviceRequest - returning err(%p)", this, (void*)err);
	return err;
}



void 
AppleUSBHub::TimeoutOccurred(OSObject *owner, IOTimerEventSource *sender)
{
    
    AppleUSBHub			*me;
    AppleUSBHubPort 	*port;
    UInt32				currentPort;
	IOReturn			kr = kIOReturnSuccess;
	bool				checkPorts = false;
	
    me = OSDynamicCast(AppleUSBHub, owner);
    if (!me)
        return;

	me->retain();
	
	// Only check for ports every kDevZeroTimeoutCount counts
	if ( --me->_devZeroLockedTimeoutCounter == 0 )
	{
		checkPorts = true;
		me->_devZeroLockedTimeoutCounter = kDevZeroTimeoutCount;
	}

    if ( checkPorts && me->_ports) 
	{
        for (currentPort = 1; currentPort <= me->_hubDescriptor.numPorts; currentPort++)
        {
			port = me->_ports ? me->_ports[currentPort-1] : NULL;
            if (port) 
            {
                Boolean	locked;
				
				port->retain();
				
                locked = port->GetDevZeroLock();
                if ( locked )
                {
                    // If the timeout flag for this port is already set, AND the timestamp is the same as when we
                    // first detected the lock, then is time to release the devZero lock
                    if ( ((me->_timeoutFlag & (1 << (currentPort-1))) != 0) && (me->_portTimeStamp[currentPort-1] == port->GetPortTimeStamp()) )
                    {
                        // Need to call through a separate thread because we will end up making synchronous calls
                        // to the USB bus.  That thread will clear the timeoutFlag for this port.
                        USBLog(3,"AppleUSBHub[%p]::TimeoutOccurred error - calling IncrementOutstandingIO", me);
                        me->IncrementOutstandingIO();
                        thread_call_enter(me->_resetPortZeroThread);
                    }
                    else
                    {
                        // Set the timeout flag for this port
                        me->_timeoutFlag |= (1<<(currentPort-1));
                        
                        // Set the timestamp for the port
                        me->_portTimeStamp[currentPort-1] = port->GetPortTimeStamp();
                    }
                }
                else
                {
                    // Port is not locked, make sure that we clear the timeoutFlag for this port and reset the timestamp
                    me->_timeoutFlag &= ~( 1<<(currentPort-1));
                    me->_portTimeStamp[currentPort-1] = 0;
                }
				
				port->release();
            }
        }
	}
	
    /*
     * Restart the watchdog timer
     */
    if (me->_timerSource && !me->isInactive() )
    {
        // me->retain();
        me->_timerSource->setTimeoutMS(kWatchdogTimerPeriod);
    }
    me->release();

}/* end timeoutOccurred */


//=============================================================================================
//
//  CallCheckForDeadHub
//  This is called by the hub driver if the interrupt pipe comes back with a NotResponding error
//  or by the hub port driver if somethine has gone wrong trying to talk to the hub (e.g. a port reset
//  fails) It increments the IO count and spins off a new thread to actually do the checking
//
//=============================================================================================
//
void
AppleUSBHub::CallCheckForDeadHub(void)
{
    IncrementOutstandingIO();
    thread_call_enter(_hubDeadCheckThread);
}



//=============================================================================================
//
//  CheckForDeadHub is called when we get a kIODeviceNotResponding error in our interrupt pipe.
//  This can mean that (1) the device was unplugged, or (2) we lost contact
//  with our hub.  In case (1), we just need to close the driver and go.  In
//  case (2), we need to ask if we are still attached.  If we are, then we update 
//  our retry count.  Once our retry count (3 from the 9 sources) are exhausted, then we
//  issue a DeviceReset to our provider, with the understanding that we will go
//  away (as an interface).
//
//=============================================================================================
//
void 
AppleUSBHub::CheckForDeadHubEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);
    
    if (!me)
        return;
     
    me->CheckForDeadHub();
    me->DecrementOutstandingIO();
}



void 
AppleUSBHub::CheckForDeadHub()
{
    IOReturn			err = kIOReturnSuccess;
    
    // Are we still connected?
    //
    if ( _device && !_hubIsDead)
    {
        err = _device->message(kIOUSBMessageHubIsDeviceConnected, NULL, 0);
    
        if ( kIOReturnSuccess == err)
        {
			// If we are connected, we want to rearm the interrupt read when appropriate, so say so
			_needInterruptRead = true;
			
			// Looks like the device is still plugged in.  Have we reached our retry count limit?
            //
            if ( --_retryCount == 0 )
            {
				retain();
				ResetMyPort();
				release();
            }
			else
			{
				USBLog(3, "AppleUSBHub[%p]::CheckForDeadHub - Still connected but retry count (%ld) not reached, clearing stall and retrying", this, _retryCount);
				
				// First, clear the halted bit in the controller and the device and re-arm the interrupt
				//
				if ( _interruptPipe )
				{
					_interruptPipe->ClearPipeStall(true);
				}
				
			}
        }
        else
        {
            // Device is not connected -- our device has gone away.  The message kIOServiceIsTerminated
            // will take care of shutting everything down.  
            //
            _hubHasBeenDisconnected = TRUE;
            USBLog(3, "AppleUSBHub[%p]::CheckForDeadHub - device has been unplugged", this);
        }
    }
    else
    {
        USBLog(3,"AppleUSBHub[%p]::CheckForDeadHub -- already resetting hub", this);
    }

}



//=============================================================================================
//
//  ClearFeatureEndpointHaltEntry is called when we get an error from our interrupt read
//  (except for kIOReturnNotResponding  which will check for a dead device).  In these cases
//  we need to clear the halted bit in the controller AND we need to reset the data toggle on the
//  device.
//
//=============================================================================================
//
void
AppleUSBHub::ClearFeatureEndpointHaltEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);

    if (!me)
        return;

    me->ClearFeatureEndpointHalt();
    me->DecrementOutstandingIO();
}



void
AppleUSBHub::ClearFeatureEndpointHalt( )
{
    IOReturn			status = kIOReturnSuccess;
    IOUSBDevRequest		request;
    UInt32			retries = 2;
    
    // Clear out the structure for the request
    //
    bzero( &request, sizeof(IOUSBDevRequest));
    
    while ( (retries > 0) && (_interruptPipe) )
    {
        retries--;
        
        // Build the USB command to clear the ENDPOINT_HALT feature for our interrupt endpoint
        //
        request.bmRequestType 	= USBmakebmRequestType(kUSBNone, kUSBStandard, kUSBEndpoint);
        request.bRequest        = kUSBRqClearFeature;
        request.wValue		= kUSBFeatureEndpointStall;
        request.wIndex		= _interruptPipe->GetEndpointNumber() | 0x80 ; // bit 7 sets the direction of the endpoint to IN
        request.wLength		= 0;
        request.pData 		= NULL;
        
        // Send the command over the control endpoint
        //
        status = _device->DeviceRequest(&request, 5000, 0);
        
        if ( status != kIOReturnSuccess )
        {
            USBLog(3, "AppleUSBHub[%p]::ClearFeatureEndpointHalt -  DeviceRequest returned: 0x%x, retries = %ld", this, status, retries);
            IOSleep(100);
        }
        else
            break;
    }
    
}



//=============================================================================================
//
//  CheckForActivePortsEntry is called when we think that we are done with the hub state machine
//  at least for a while. If all ports are either disconnected or suspended, then we will call
//	changePowerStateToPriv to lower the state. Note that the ports themselves have a separate
//	power state by virtue of changePowerStateTo
//=============================================================================================
//
void
AppleUSBHub::CheckForActivePortsEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);
	
    if (!me)
        return;
	
    me->CheckForActivePorts();
	me->_checkPortsThreadActive = false;
	me->release();
}



void
AppleUSBHub::CheckForActivePorts( )
{
	// If this hub doesn't allow low power mode, then just return
	if ( _dontAllowLowPower )
		return;
	
	USBLog(7, "AppleUSBHub[%p]::CheckForActivePorts - checking for disconnected ports", this);
	if (HubAreAllPortsDisconnectedOrSuspended())
	{
		USBLog(4, "AppleUSBHub[%p]::CheckForActivePorts - lowering power state", this);
		_dozeEnabled = true;
		changePowerStateToPriv(kIOUSBHubPowerStateLowPower);
		// note that the state could go back up with a call to EnsureUsability
	}
}



//=============================================================================================
//
//  WaitForPortResumesEntry is called when we are waking up from sleep, and we have resume calls
//	outstanding on ports which had been suspended by the power manager. since we are not yet
//  in the ON state, we don't have an interrupt read pending, so we can't let the port status
//	change handler deal with it. Instead we poll the individual port status change bits
//	ourselves until all ports are in the active state..
//=============================================================================================
//
void
AppleUSBHub::WaitForPortResumesEntry(OSObject *target)
{
    AppleUSBHub *	me = OSDynamicCast(AppleUSBHub, target);
	
    if (!me)
        return;
	
    me->WaitForPortResumes();
}



void
AppleUSBHub::WaitForPortResumes( )
{
	int						portIndex;
	int						portNum;
	bool					needToWait = true;
    AppleUSBHubPort *		port;
	IOUSBHubPortStatus		portStatus;
	IOReturn				kr;
	int						innerRetries = 30;						// wait at most 30 milliseconds (50% margin)
	int						outerRetries = 100;						// total wait of 3 seconds
	bool					dealWithPort[_hubDescriptor.numPorts];
	bool					recoveryNeeded = true;

	if (!_ports)
		return;
	
	for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
		dealWithPort[portIndex] = false;
	
	USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - checking for ports which need to be resumed", this);
	while (needToWait && (outerRetries-- > 0))
	{
		innerRetries = 30;
		while (needToWait && (innerRetries-- > 0))
		{
			needToWait = false;							// try to get out of here...
			for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
			{
				portNum = portIndex+1;
				port = _ports[portIndex];
				if (port)
				{
					if (port->_portPMState == usbHPPMS_pm_suspended)
					{
						USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - port[%p] which is port number (%d) still waiting", this, port, portNum);
						portStatus.statusFlags = 0;
						portStatus.changeFlags = 0;
						kr = GetPortStatus(&portStatus, portNum);
						if (kr)
						{
							USBLog(1, "AppleUSBHub[%p]::WaitForPortResumes - err (%p) from GetPortStatus for port (%d) - setting port to active", this, (void*)kr, portNum);
							dealWithPort[portIndex] = true;
							port->_portPMState = usbHPPMS_active;
							port->_resumePending = false;
						}
						if (portStatus.changeFlags & kHubPortSuspend)
						{
							ClearPortFeature(kUSBHubPortSuspendChangeFeature, portNum);
							dealWithPort[portIndex] = true;
							USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - port[%p], which is number[%d] has the suspend change bit set- good - statusFlags[%p] and changeFlags[%p]", this, port, portNum, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags);
							port->_portPMState = usbHPPMS_active;
							port->_resumePending = false;
						}
						else
						{
							USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - port[%p], which is number(%d) has statusFlags[%p] and changeFlags[%p]", this, port, portNum, (void*)portStatus.statusFlags, (void*)portStatus.changeFlags);
							needToWait = true;
						}
					}
				}
			}
			// this is still in the inner loop - wait 1 ms between retries
			if (needToWait)
				IOSleep(1);
			
		}
		if (needToWait)
		{
			needToWait = false;
			// this is out of the inner loop. if needToWait is till set, then it appears that some port is not getting cleared for some reason
			for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
			{
				portNum = portIndex+1;
				port = _ports[portIndex];
				if (port)
				{
					if (port->_portPMState == usbHPPMS_pm_suspended)
					{
						USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - port[%p] which is port number (%d) still waiting", this, port, portNum);
						portStatus.statusFlags = 0;
						portStatus.changeFlags = 0;
						kr = GetPortStatus(&portStatus, portNum);
						if (kr)
						{
							USBLog(1, "AppleUSBHub[%p]::WaitForPortResumes - err (%p) from GetPortStatus for port (%d) - setting port to active", this, (void*)kr, portNum);
							dealWithPort[portIndex] = true;
							port->_portPMState = usbHPPMS_active;
							port->_resumePending = false;
						}
						else
						{
							if (portStatus.changeFlags & kHubPortSuspend)
							{
								USBLog(3, "AppleUSBHub[%p]::WaitForPortResumes - port[%p] which is port number (%d) has the suspend change set in the OUTER loop", this, port, portNum);
								needToWait = true;					// to send us back up
							}
							else if (portStatus.statusFlags & kHubPortSuspend)
							{
								// reissue the command which was issued earlier and apparently didn't stick
								kr = port->SuspendPort(false, false);					
								if (kr)
								{
									USBLog(1, "AppleUSBHub[%p]::WaitForPortResumes - err (%p) from SuspendPort for port (%d)", this, (void*)kr, portNum);
									dealWithPort[portIndex] = true;
									port->_portPMState = usbHPPMS_active;		// give up on this port i guess
									port->_resumePending = false;
								}
								else
								{
									needToWait = true;					// to send us back up
								}
							}
						}
					}
				}
			}
		}
	}
	// now deal with any ports which were marked
	for (portIndex = 0; portIndex < _hubDescriptor.numPorts; portIndex++)
	{
		portNum = portIndex + 1;
		port = _ports[portIndex];
		if (port && dealWithPort[portIndex])
		{
			IOUSBControllerV3		*v3Bus = NULL;
			IOReturn				err;
			
			if (recoveryNeeded)
			{
				// according to the USB 2.0 spec, when resuming, we need to allow 10 ms recover time before we talk to devices
				// on the bus. so don't enable the endpoints until that recovery is over. this will apply to all ports which
				// we have recently enabled
				recoveryNeeded = false;
				IOSleep(port->_portResumeRecoveryTime);
			}
			if (_device)
				v3Bus = OSDynamicCast(IOUSBControllerV3, _bus);
			
			if (v3Bus && port->_portDevice)
			{
				USBLog(5, "AppleUSBHub[%p]::WaitForPortResumes - Enabling endpoints for device at address (%d)", this, (int)port->_portDevice->GetAddress());
				err = v3Bus->EnableAddressEndpoints(port->_portDevice->GetAddress(), true);
				if (err)
				{
					USBLog(2, "AppleUSBHub[%p]::WaitForPortResumes - EnableAddressEndpoints returned (%p)", this, (void*)err);
				}
			}
			// these are to counteract the Increments in StartPorts
			// note that this needs to be done AFTER we re-enable the Address endpoints
			DecrementOutstandingIO();
			DecrementOutstandingResumes();
		}
	}
}



void 
AppleUSBHub::ResetMyPort()
{
	IOReturn				err;
    int						currentPort;
    AppleUSBHubPort *		port;
	
	USBLog(3, "AppleUSBHub[%p]::ResetMyPort - marking hub as dead", this);	
	_hubIsDead = TRUE;
	
    if ( _interruptPipe )
    {
		err = _interruptPipe->Abort();
        if ( err != kIOReturnSuccess )
        {
            USBLog(1, "AppleUSBHub[%p]::ResetMyPort interruptPipe->Abort returned %p", this, (void*)err);
        }
    }
	
    if ( _ports)
    {
        for (currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
        {
            port = _ports ? _ports[currentPort-1] : NULL;
            if (port)
            {
                if (port->_devZero)
                {
                    USBLog(1, "AppleUSBHub[%p]::StopPorts - port %d had the dev zero lock", this, currentPort);
                }
                port->ReleaseDevZeroLock();
            }
        }
    }
	
    // If our timerSource is going, cancel it, as we don't need to
    // timeout our ports anymore
    //
    if (_timerSource) 
    {
        _timerSource->cancelTimeout();
    }

 	// We need to call resetDevice once any power changes are done, so we set up a flag here and check it in powerChangeDone and
	// do the reset there.
	USBLog(3, "AppleUSBHub[%p]::ResetMyPort  calling changePowerStateToPriv(kIOUSBHubPowerStateOff) %d", this, kIOUSBHubPowerStateOff);
	_needToCallResetDevice = true;
	changePowerStateToPriv(kIOUSBHubPowerStateOn);
	powerOverrideOnPriv();
	changePowerStateToPriv(kIOUSBHubPowerStateOff);

}



bool
AppleUSBHub::IsHSRootHub()
{
    if (_hsHub && _isRootHub)
		return true;
    else
		return false;
}



#pragma mark �������� Bookkeeping ���������
void
AppleUSBHub::DecrementOutstandingIO(void)
{
	UInt32			outstandingIO;
	IOReturn		err;
	static			int		gSerial = 0;
	int				localSerial;
	
	localSerial = gSerial++;
    if (!_gate)
    {
		USBLog(5, "+AppleUSBHub[%p]::DecrementOutstandingIO(%d) isInactive = %d, outstandingIO = %ld, _needInterruptRead: %d - no gate", this, localSerial, isInactive(), _outstandingIO, _needInterruptRead);
		outstandingIO = --_outstandingIO;
    }
	else
	{
		err = _gate->runAction(ChangeOutstandingIO, (void*)-1, &outstandingIO);
		USBLog(5, "AppleUSBHub[%p]::DecrementOutstandingIO(%d) isInactive(%s), gated call returned err (%p) count (%d), _needInterruptRead(%d)", this, localSerial, isInactive() ? "true" : "false", (void*)err, (int)outstandingIO, _needInterruptRead);
	}

	if ( IsPortInitThreadActiveForAnyPort() )
	{
		// We want to rearm the read if any port has the devZero lock, as they are probably waiting for a port status
		if ((_myPowerState == kIOUSBHubPowerStateOn) && _needInterruptRead)
		{
			// even though i may lower my power state below, it may not actually get lowered until our children are all at a lower state
			// so go ahead and schedule another interrupt read
			USBLog(5, "AppleUSBHub[%p]::DecrementOutstandingIO(%d), rearming read because devZero lock was held by a port", this, localSerial);
			_needInterruptRead = false;
			RearmInterruptRead();
		}
	}
	else if (!outstandingIO)
	{
		if (_needToClose)
		{
			if ((_myPowerState == kIOUSBHubPowerStateOn) && (IOLockTryLock(_doPortActionLock)))
			{
				_needToClose = false;						// so that we don't do this twice..
				USBLog(3, "AppleUSBHub[%p]::DecrementOutstandingIO(%d) isInactive(%s) outstandingIO(%d) _powerStateChangingTo(%d) - closing device", this, localSerial, isInactive() ? "true" : "false", (int)outstandingIO, (int)_powerStateChangingTo);
				// Stop/close all ports, deallocate our ports
				//
				StopPorts();
				IOLockUnlock(_doPortActionLock);
				_device->close(this);
			}
			else
			{
				USBLog(3, "AppleUSBHub[%p]::DecrementOutstandingIO(%d) _needToClose(true) but waiting for _myPowerState == kIOUSBHubPowerStateOn(%d) or for the _doPortActionLock", this, localSerial, (int)_myPowerState);
			}
		}
		else 
		{
			if ( (_myPowerState == kIOUSBHubPowerStateOn) && !_hubIsDead && !isInactive())
			{
				// even though i may lower my power state below, it may not actually get lowered until our children are all at a lower state
				// so go ahead and schedule another interrupt read
				if (_needInterruptRead)
				{
					USBLog(3, "AppleUSBHub[%p]::DecrementOutstandingIO(%d), outstandingIO = %ld - rearming read", this, localSerial, outstandingIO);
					_needInterruptRead = false;
					RearmInterruptRead();
				}
				if (!_checkPortsThreadActive)
				{
					_checkPortsThreadActive = true;
					retain();
					thread_call_enter(_checkForActivePortsThread);
				}
			}
		}
	}
	USBLog(5, "-AppleUSBHub[%p]::DecrementOutstandingIO(%d)", this, localSerial);
}



void
AppleUSBHub::IncrementOutstandingIO(void)
{
    if (!_gate)
    {
		USBLog(5, "AppleUSBHub[%p]::IncrementOutstandingIO isInactive = %d, outstandingIO = %ld - no gate", this, isInactive(), _outstandingIO);
		_outstandingIO++;
		return;
    }
    _gate->runAction(ChangeOutstandingIO, (void*)1);
}



IOReturn
AppleUSBHub::ChangeOutstandingIO(OSObject *target, void *param1, void *param2, void *param3, void *param4)
{
    AppleUSBHub		*me = OSDynamicCast(AppleUSBHub, target);
    UInt32			direction = (UInt32)param1;
	UInt32			*retCount = (UInt32*)param2;
	IOReturn		ret = kIOReturnSuccess;
    
    if (!me)
    {
		USBLog(1, "AppleUSBHub::ChangeOutstandingIO - invalid target");
		return kIOReturnBadArgument;
    }
    switch (direction)
    {
		case 1:
			me->_outstandingIO++;
			USBLog(5, "AppleUSBHub[%p]::ChangeOutstandingIO(+) now (%d)", me, (int)me->_outstandingIO);
			break;
			
		case -1:
			--me->_outstandingIO;
			USBLog(5, "AppleUSBHub[%p]::ChangeOutstandingIO(-) now (%d)", me, (int)me->_outstandingIO);
			break;
			
		default:
			USBLog(1, "AppleUSBHub[%p]::ChangeOutstandingIO - invalid direction", me);
			ret = kIOReturnBadArgument;
    }
	
	if (retCount)
		*retCount = me->_outstandingIO;
		
    return ret;
}



void
AppleUSBHub::LowerPowerState(void)
{
	UInt32			raisedPowerStateCount;
	IOReturn		err;
	static			int		gSerial = 0;
	int				localSerial;
	
	localSerial = gSerial++;
    if (!_gate)
    {
		USBLog(5, "+AppleUSBHub[%p]::LowerPowerState(%d) isInactive = %d, _raisedPowerStateCount = %ld - no gate", this, localSerial, isInactive(), _raisedPowerStateCount);
		raisedPowerStateCount = --_raisedPowerStateCount;
    }
	else
	{
		err = _gate->runAction(ChangeRaisedPowerState, (void*)-1, &raisedPowerStateCount);
		USBLog(5, "AppleUSBHub[%p]::LowerPowerState(%d) isInactive(%s), gated call returned err (%p) count (%d)", this, localSerial, isInactive() ? "true" : "false", (void*)err, (int)raisedPowerStateCount);
	}

	if (!raisedPowerStateCount)
	{
		changePowerStateTo(kIOUSBHubPowerStateLowPower);
	}	
	USBLog(5, "-AppleUSBHub[%p]::LowerPowerState(%d)", this, localSerial);
}



void
AppleUSBHub::RaisePowerState(void)
{
	IOReturn		err;
	UInt32			raisedPowerStateCount;

    if (!_gate)
    {
		USBLog(5, "AppleUSBHub[%p]::RaisePowerState isInactive = %d, outstandingIO = %ld - no gate", this, isInactive(), _outstandingIO);
		raisedPowerStateCount = _raisedPowerStateCount++;
    }
	else
	{
		err = _gate->runAction(ChangeRaisedPowerState, (void*)1, &raisedPowerStateCount);
		USBLog(5, "AppleUSBHub[%p]::RaisePowerState isInactive(%s), gated call returned err (%p) count (%d)", this, isInactive() ? "true" : "false", (void*)err, (int)raisedPowerStateCount);
	}
	if (raisedPowerStateCount == 1)				// only need to do this once
	{
		changePowerStateTo(kIOUSBHubPowerStateOn);
	}
}



IOReturn
AppleUSBHub::ChangeRaisedPowerState(OSObject *target, void *param1, void *param2, void *param3, void *param4)
{
    AppleUSBHub		*me = OSDynamicCast(AppleUSBHub, target);
    UInt32			direction = (UInt32)param1;
	UInt32			*retCount = (UInt32*)param2;
	IOReturn		ret = kIOReturnSuccess;
    
    if (!me)
    {
		USBLog(1, "AppleUSBHub::ChangeRaisedPowerState - invalid target");
		return kIOReturnBadArgument;
    }
    switch (direction)
    {
		case 1:
			me->_raisedPowerStateCount++;
			USBLog(3, "AppleUSBHub[%p]::ChangeRaisedPowerState(+) now (%d)", me, (int)me->_raisedPowerStateCount);
			break;
			
		case -1:
			--me->_raisedPowerStateCount;
			USBLog(3, "AppleUSBHub[%p]::ChangeRaisedPowerState(-) now (%d)", me, (int)me->_raisedPowerStateCount);
			break;
			
		default:
			USBLog(1, "AppleUSBHub[%p]::ChangeRaisedPowerState - invalid direction", me);
			ret = kIOReturnBadArgument;
    }
	
	if (retCount)
		*retCount = me->_raisedPowerStateCount;
		
    return ret;
}



void
AppleUSBHub::DecrementOutstandingResumes(void)
{
	UInt32			outstandingResumes;
	IOReturn		err;
	static			int		gSerial = 0;
	int				localSerial;
	
	localSerial = gSerial++;
    if (!_gate)
    {
		USBLog(5, "AppleUSBHub[%p]::+DecrementOutstandingResumes(%d) isInactive = %d, outstandingIO = %ld, _needInterruptRead: %d - no gate", this, localSerial, isInactive(), _outstandingIO, _needInterruptRead);
		outstandingResumes = --_outstandingResumes;
    }
	else
	{
		err = _gate->runAction(ChangeOutstandingResumes, (void*)-1, &outstandingResumes);
		USBLog(5, "AppleUSBHub[%p]::DecrementOutstandingResumes(%d) isInactive(%s), gated call returned err (%p) count (%d)", this, localSerial, isInactive() ? "true" : "false", (void*)err, (int)outstandingResumes);
	}
	
	if (!outstandingResumes)
	{
		USBLog(5, "AppleUSBHub[%p]::DecrementOutstandingResumes(%d) - resumes down to zero", this, localSerial);
		if (_needToAckSetPowerState)
		{
			USBLog(5, "AppleUSBHub[%p]::DecrementOutstandingResumes(%d) - calling acknowledgeSetPowerState", this, localSerial);
			_needToAckSetPowerState = false;
			acknowledgeSetPowerState();
		}
	}
	
	USBLog(5, "AppleUSBHub[%p]::-DecrementOutstandingResumes(%d)", this, localSerial);
}



void
AppleUSBHub::IncrementOutstandingResumes(void)
{
    if (!_gate)
    {
		USBLog(5, "AppleUSBHub[%p]::IncrementOutstandingResumes isInactive = %d, outstandingIO = %ld - no gate", this, isInactive(), _outstandingResumes);
		_outstandingResumes++;
		return;
    }
    _gate->runAction(ChangeOutstandingResumes, (void*)1);
}



IOReturn
AppleUSBHub::ChangeOutstandingResumes(OSObject *target, void *param1, void *param2, void *param3, void *param4)
{
    AppleUSBHub		*me = OSDynamicCast(AppleUSBHub, target);
    UInt32			direction = (UInt32)param1;
	UInt32			*retCount = (UInt32*)param2;
	IOReturn		ret = kIOReturnSuccess;
    
    if (!me)
    {
		USBLog(1, "AppleUSBHub::ChangeOutstandingResumes - invalid target");
		return kIOReturnBadArgument;
    }
    switch (direction)
    {
		case 1:
			me->_outstandingResumes++;
			USBLog(3, "AppleUSBHub[%p]::ChangeOutstandingResumes(+) now (%d)", me, (int)me->_outstandingResumes);
			break;
			
		case -1:
			--me->_outstandingResumes;
			USBLog(3, "AppleUSBHub[%p]::ChangeOutstandingResumes(-) now (%d)", me, (int)me->_outstandingResumes);
			break;
			
		default:
			USBLog(1, "AppleUSBHub[%p]::ChangeOutstandingResumes - invalid direction", me);
			ret = kIOReturnBadArgument;
    }
	
	if (retCount)
		*retCount = me->_outstandingResumes;
	
    return ret;
}



#pragma mark �������� User Client ���������
IOReturn
AppleUSBHub::EnterTestMode()
{
    IOUSBControllerV2 	*con;
    int					currentPort;
    AppleUSBHubPort		*port;
    
    if (!_hsHub)
		return kIOReturnBadArgument;
	
    if (_isRootHub)
    {
		con = OSDynamicCast(IOUSBControllerV2, _bus);
		if (!con)
			return kIOReturnBadArgument;
	    
		USBLog(1, "AppleUSBHub[%p]::EnterTestMode - root hub", this);    
		_inTestMode = true;
		return con->SetTestMode(kEHCITestMode_Start, 0);
    }
    // not a root hub
    _inTestMode = true;
    if ( _ports)
    {
		USBLog(1, "AppleUSBHub[%p]::EnterTestMode - external hub - suspending ports", this);    
        for (currentPort = 0; currentPort < _hubDescriptor.numPorts; currentPort++)
        {
            port = _ports[currentPort];
            if (port)
            {
				port->SuspendPort(true, false);
            }
        }
    }
    return kIOReturnSuccess;
}



IOReturn
AppleUSBHub::LeaveTestMode()
{
    IOUSBControllerV2 	*con;

    if (!_hsHub)
		return kIOReturnBadArgument;

    if (_isRootHub)
    {
		con = OSDynamicCast(IOUSBControllerV2, _bus);
		if (!con)
			return kIOReturnBadArgument;
	    
		USBLog(1, "AppleUSBHub[%p]::LeaveTestMode - root hub", this);    
		return con->SetTestMode(kEHCITestMode_End, 0);
    }
    // not a root hub - just reset my port - this will terminate me
    USBLog(1, "AppleUSBHub[%p]::LeaveTestMode - external hub", this);    
    retain();
    ResetMyPort();
    release();
    return kIOReturnSuccess;
}


IOReturn
AppleUSBHub::PutPortIntoTestMode(UInt32 port, UInt32 mode)
{
    IOUSBDevRequest	request;

    if (!_hsHub || !_inTestMode)
		return kIOReturnBadArgument;

    if (_isRootHub)
    {
		USBLog(1, "AppleUSBHub[%p]::PutPortIntoTestMode - putting root hub port %ld into mode %lx", this, port, mode);    
		return _bus->SetTestMode(mode, port);
    }
    
    USBLog(1, "AppleUSBHub[%p]::PutPortIntoTestMode - putting external hub port %ld into mode %lx", this, port, mode);    
    return SetPortFeature(kUSBHubPortTestFeature, (mode << 8) + port);
}

//================================================================================================
//   SetIndicatorForPort
//================================================================================================
IOReturn
AppleUSBHub::SetIndicatorForPort(UInt16 port, UInt16 selector)
{
	IOReturn		kr = kIOReturnUnsupported;
    IOUSBDevRequest	request;
	
    USBLog(5, "AppleUSBHub[%p](0x%lx)::SetIndicatorForPort port %d, selector %d", this, _locationID, port, selector);
	
	return SetPortFeature(kUSBHubPortIndicatorFeature, (selector << 8) + port);
}

//================================================================================================
//   GetIndicatorForPort
//================================================================================================

IOReturn
AppleUSBHub::GetPortIndicatorControl(UInt16 port, UInt32 *defaultColors)
{
	IOReturn			kr = kIOReturnUnsupported;
	IOUSBHubPortStatus	portStatus;
	
	kr = GetPortStatus(&portStatus, port);
    if ( kIOReturnSuccess != kr )
    {
        USBLog(1, "AppleUSBHub[%p](0x%lx)::GetPortIndicatorControl  GetPortStatus to port %d got error (0x%x) from DoDeviceRequest", this, _locationID, port, kr);
    }
	else
	{
		if ( portStatus.statusFlags & kHubPortIndicator )
		{
			USBLog(6, "AppleUSBHub[%p](0x%lx)::GetPortIndicatorControl - port %d indicators are under software control", this, _locationID, port);
			*defaultColors = 1;
		}
		else
		{
			USBLog(6, "AppleUSBHub[%p](0x%lx)::GetPortIndicatorControl - port %d indicators display default colors", this, _locationID, port);
			*defaultColors = 0;
		}
	}
	
	return kr;
}

//================================================================================================
//   SetIndicatorsToAutomatic
//================================================================================================

IOReturn
AppleUSBHub::SetIndicatorsToAutomatic()
{
	IOReturn		kr = kIOReturnUnsupported;
	
    USBLog(5, "AppleUSBHub[%p](0x%lx)::SetIndicatorsToAutomatic", this, _locationID);
	
	for (int currentPort = 1; currentPort <= _hubDescriptor.numPorts; currentPort++)
	{
		kr = SetIndicatorForPort(currentPort, kHubPortIndicatorAutomatic);
		if ( kIOReturnSuccess != kr )
		{
			USBLog(1, "AppleUSBHub[%p](0x%lx)::SetIndicatorForPort to port %d got error (0x%x)", this, _locationID, currentPort, kr);
		}
	}
	
	return kr;
}

//================================================================================================
//   GetPortPower
//================================================================================================

IOReturn
AppleUSBHub::GetPortPower(UInt16 port, UInt32 *on)
{
	IOReturn			kr = kIOReturnUnsupported;
	IOUSBHubPortStatus	portStatus;
	
	kr = GetPortStatus(&portStatus, port);
    if ( kIOReturnSuccess != kr )
    {
        USBLog(1, "AppleUSBHub[%p](0x%lx)::GetPortPower  GetPortStatus to port %d got error (0x%x) from DoDeviceRequest", this, _locationID, port, kr);
    }
	else
	{
		if ( portStatus.statusFlags & kHubPortPower )
		{
			USBLog(6, "AppleUSBHub[%p](0x%lx)::GetPortIndicatorControl - port %d is NOT in the Powered-off state", this, _locationID, port);
			*on = 1;
		}
		else
		{
			USBLog(6, "AppleUSBHub[%p](0x%lx)::GetPortIndicatorControl - port %d is in the Powered-off state", this, _locationID, port);
			*on = 0;
		}
	}
	
    USBLog(5, "AppleUSBHub[%p](0x%lx)::GetPortPower port %d returning on = %ld", this, _locationID, port, *on);
	
	return kr;
}

//================================================================================================
//   SetPortPower
//================================================================================================

IOReturn
AppleUSBHub::SetPortPower(UInt16 port, UInt32 on)
{
	IOReturn		kr = kIOReturnUnsupported;
	
    USBLog(5, "AppleUSBHub[%p](0x%lx)::SetPortPower to %s, for port %d", this, _locationID, on ? "ON" : "OFF", port);
	
	if ( on == 1 )
		return SetPortFeature(kUSBHubPortPowerFeature, port);
	else
		return ClearPortFeature(kUSBHubPortPowerFeature, port);
}



const char *	
AppleUSBHub::HubMessageToString(UInt32 message)
{
	switch (message)
	{
		case kIOUSBMessageHubIsDeviceConnected:
			return "kIOUSBMessageHubIsDeviceConnected";
		case kIOUSBMessageHubSuspendPort:
			return "kIOUSBMessageHubSuspendPort";
		case kIOUSBMessageHubResumePort:
			return "kIOUSBMessageHubResumePort";
		case kIOUSBMessageHubResetPort:
			return "kIOUSBMessageHubResetPort";
		case kIOUSBMessageHubPortClearTT:
			return "kIOUSBMessageHubPortClearTT";
		case kIOUSBMessageHubSetPortRecoveryTime:
			return "kIOUSBMessageHubSetPortRecoveryTime";
		case kIOUSBMessageHubReEnumeratePort:
			return "kIOUSBMessageHubReEnumeratePort";
		case kIOMessageServiceIsTerminated: 	
			return "kIOMessageServiceIsTerminated";
		case kIOUSBMessagePortHasBeenReset:
			return "kIOUSBMessagePortHasBeenReset";
		case kIOUSBMessagePortHasBeenResumed:
			return "kIOUSBMessagePortHasBeenResumed";
		case kIOUSBMessagePortWasNotSuspended:
			return "kIOUSBMessagePortWasNotSuspended";
		case kIOMessageServiceIsRequestingClose:
			return "kIOMessageServiceIsRequestingClose";
		case kIOMessageServiceIsAttemptingOpen:
			return "kIOMessageServiceIsAttemptingOpen";
		case kIOUSBMessagePortHasBeenSuspended:
			return "kIOUSBMessagePortHasBeenSuspended";
		default:
			return "UNKNOWN";
	}
}
