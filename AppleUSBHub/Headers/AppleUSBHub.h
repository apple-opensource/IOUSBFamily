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

#ifndef _IOKIT_APPLEUSBHUB_H
#define _IOKIT_APPLEUSBHUB_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>

#include <IOKit/usb/USB.h>
#include <IOKit/usb/USBHub.h>
#include <IOKit/usb/IOUSBLog.h>
#include <IOKit/usb/IOUSBRootHubDevice.h>

#include <kern/thread_call.h>

/* Convert USBLog to use kprintf debugging */
#ifndef APPLEUSBHUB_USE_KPRINTF
	#define APPLEUSBHUB_USE_KPRINTF 0
#endif

#if APPLEUSBHUB_USE_KPRINTF
#undef USBLog
#undef USBError
void kprintf(const char *format, ...)
__attribute__((format(printf, 1, 2)));
#define USBLog( LEVEL, FORMAT, ARGS... )  if ((LEVEL) <= 5) { kprintf( FORMAT "\n", ## ARGS ) ; }
#define USBError( LEVEL, FORMAT, ARGS... )  { kprintf( FORMAT "\n", ## ARGS ) ; }
#endif

enum{
      kErrataCaptiveOKBit = 0x01,
      kStartupDelayBit =	0x02,
	  kExtraPowerPossible = 0x04
};

class IOUSBController;
class IOUSBDevice;
class IOUSBInterface;
class IOUSBPipe;
class AppleUSBHubPort;


class AppleUSBHub : public IOService
{
    OSDeclareDefaultStructors(AppleUSBHub)

    friend class AppleUSBHubPort;
    friend class AppleUSBHSHubUserClient;

    IOUSBController *				_bus;
    IOUSBDevice *					_device;
    IOUSBInterface *				_hubInterface;
    IOUSBConfigurationDescriptor	*_configDescriptor;
    IOUSBHubDescriptor				_hubDescriptor;
    USBDeviceAddress				_address;
    IOUSBHubPortStatus				_hubStatus;
    IOUSBPipe *						_interruptPipe;
    IOBufferMemoryDescriptor *		_buffer;
    IOCommandGate *					_gate;
    IOWorkLoop *					_workLoop;
    UInt32							_locationID;
    UInt32							_inStartMethod;
	UInt32							_devZeroLockedTimeoutCounter;					// We use this to count down to see when we need to check for a possible stuck dev zero lock
    bool							_portSuspended;
    bool							_hubHasBeenDisconnected;
    bool							_hubIsDead;
	bool							_abortExpected;
	UInt32							_retryCount;
    IOUSBRootHubDevice *			_rootHubParent;									// set if our hub is attached to a root hub
	
    // Power stuff
    bool							_busPowered;
    bool							_selfPowered;
    bool							_busPowerGood;
    bool							_selfPowerGood;
	AppleRootHubExtraPowerRequest	_extraPower;									// request from a root hub due to a property
	UInt32							_extraPowerPorts;								// from a property
	UInt32							_extraPowerRemaining;							// how many milliamps we can still give to any one port
		
	// bookkeeping
    bool							_needToClose;
    
    UInt32							_powerForCaptive;
    thread_call_t					_workThread;
    thread_call_t					_resetPortZeroThread;
    thread_call_t					_hubDeadCheckThread;
    thread_call_t					_clearFeatureEndpointHaltThread;

    // Port stuff
    UInt8							_readBytes;
    UInt8							_numCaptive;
    AppleUSBHubPort **				_ports;						// Allocated at runtime
    bool							_multiTTs;					// Hub is multiTT capable, and configured.
    bool							_hsHub;						// our provider is a HS bus
    bool							_isRootHub;					// we are driving a root hub (needed for test mode)
    bool							_inTestMode;				// T while we are in test mode
    IOTimerEventSource *			_timerSource;
    UInt32							_timeoutFlag;
    UInt32							_portTimeStamp[32];
    UInt32							_portWithDevZeroLock;
    UInt32							_outstandingIO;

    // Errata stuff
    UInt32							_errataBits;
    UInt32							_startupDelay;
    
    static void 	InterruptReadHandlerEntry(OSObject *target, void *param, IOReturn status, UInt32 bufferSizeRemaining);
    void			InterruptReadHandler(IOReturn status, UInt32 bufferSizeRemaining);
    
    static void 	ProcessStatusChangedEntry(OSObject *target);
    void			ProcessStatusChanged(void);

    static void		ResetPortZeroEntry(OSObject *target);
    void			ResetPortZero();
    
    static void 	CheckForDeadHubEntry(OSObject *target);
    void			CheckForDeadHub();

    static void		ClearFeatureEndpointHaltEntry(OSObject *target);
    void			ClearFeatureEndpointHalt(void);

    static void 	TimeoutOccurred(OSObject *owner, IOTimerEventSource *sender);

    IOReturn 		DoDeviceRequest(IOUSBDevRequest *request);
    UInt32			GetHubErrataBits(void);

	// bookkeeping
    void			DecrementOutstandingIO(void);
    void			IncrementOutstandingIO(void);
    static IOReturn	ChangeOutstandingIO(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);

    // Hub functions
    void			UnpackPortFlags(void);
    void			CountCaptivePorts(void);
    IOReturn		CheckPortPowerRequirements(void);
    IOReturn		AllocatePortMemory(void);
    IOReturn		StartPorts(void);
    IOReturn 		StopPorts(void);
    IOReturn		ConfigureHub(void);

    bool			HubStatusChanged(void);

    IOReturn		GetHubDescriptor(IOUSBHubDescriptor *desc);
    IOReturn		GetHubStatus(IOUSBHubStatus *status);
    IOReturn		ClearHubFeature(UInt16 feature);

    IOReturn		GetPortStatus(IOUSBHubPortStatus *status, UInt16 port);
    IOReturn		GetPortState(UInt8 *state, UInt16 port);
    IOReturn		SetPortFeature(UInt16 feature, UInt16 port);
    IOReturn		ClearPortFeature(UInt16 feature, UInt16 port);

    void			PrintHubDescriptor(IOUSBHubDescriptor *desc);

    void			FatalError(IOReturn err, char *str);
    IOReturn		DoPortAction(UInt32 type, UInt32 portNumber, UInt32 options );
    void			StartWatchdogTimer();
    void			StopWatchdogTimer();
    IOReturn		RearmInterruptRead();
    void			ResetMyPort();
    void			CallCheckForDeadHub(void);

    IOUSBHubDescriptor 	GetCachedHubDescriptor() { return _hubDescriptor; }
    bool				MergeDictionaryIntoProvider(IOService *  provider, OSDictionary *  mergeDict);
    bool				MergeDictionaryIntoDictionary(OSDictionary *  sourceDictionary,  OSDictionary *  targetDictionary);
	bool				HubAreAllPortsDisconnectedOrSuspended();
	
	// new power stuff
	void				AllocateExtraPower();						// used at init time
	IOReturn			GetExtraPortPower(AppleUSBHubPort *port);
	IOReturn			ReturnExtraPortPower(AppleUSBHubPort *port);
    
    // test mode functions, called by the AppleUSBHSHubUserClient
    IOReturn			EnterTestMode();
    IOReturn			LeaveTestMode();
    bool				IsHSRootHub();
    IOReturn			PutPortIntoTestMode(UInt32 port, UInt32 mode);
	
	// Port Indicator and Port Power functions, called by the AppleUSBHSHubUserClient
	IOReturn			SetIndicatorForPort(UInt16 port, UInt16 selector);
	IOReturn			GetPortIndicatorControl(UInt16 port, UInt32 *defaultColors);
    IOReturn			SetIndicatorsToAutomatic();
	IOReturn			GetPortPower(UInt16 port, UInt32 *on);
	IOReturn			SetPortPower(UInt16 port, UInt32 on);
	
public:

    virtual bool	init(OSDictionary * propTable );
    virtual bool	start(IOService * provider);
    virtual void 	stop(IOService *  provider);
    virtual bool 	finalize(IOOptionBits options);
    virtual IOReturn 	message( UInt32 type, IOService * provider,  void * argument = 0 );

    // "new" IOKit methods. Some of these may go away before we ship 1.8.5
    virtual bool 	willTerminate( IOService * provider, IOOptionBits options );
    virtual bool 	didTerminate( IOService * provider, IOOptionBits options, bool * defer );
    virtual bool 	requestTerminate( IOService * provider, IOOptionBits options );
    virtual bool 	terminate( IOOptionBits options = 0 );
    virtual void 	free( void );
    virtual bool 	terminateClient( IOService * client, IOOptionBits options );

    virtual IOUSBDevice * GetDevice(void) { return _device; }

};


#endif _IOKIT_APPLEUSBHUB_H
