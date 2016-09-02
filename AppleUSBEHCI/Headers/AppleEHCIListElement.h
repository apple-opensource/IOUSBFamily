/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1998-2003 Apple Computer, Inc.  All Rights Reserved.
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


#ifndef _IOKIT_AppleEHCIListElement_H
#define _IOKIT_AppleEHCIListElement_H

#include <libkern/c++/OSObject.h>
#include <IOKit/usb/IOUSBControllerListElement.h>

#include "AppleUSBEHCI.h"
#include "USBEHCI.h"

/*
	// I'm (barry) getting confused by all these bits of structures scattered
	// all over the place, so here's my map of what's in a queue head.

	AppleEHCIQueueHead (this file)
		AppleEHCIListElement (This file)
			_sharedPhysical->
			_sharedLogical->
		EHCIQueueHeadSharedPtr GetSharedLogical(void);  (USBEHCI.h)
			-> EHCIQueueHeadShared (USBEHCI.h)
				IOPhysicalAddress CurrqTDPtr;
				IOPhysicalAddress NextqTDPtr; 
				IOPhysicalAddress AltqTDPtr;

		EHCIGeneralTransferDescriptorPtr qTD;  (AppleUSBEHCI.h)
		EHCIGeneralTransferDescriptorPtr TailTD;
			->EHCIGeneralTransferDescriptor (AppleUSBEHCI.h)
				EHCIGeneralTransferDescriptorSharedPtr pShared;
					->EHCIGeneralTransferDescriptorShared (USBEHCI.h)
						IOPhysicalAddress nextTD;
						IOPhysicalAddress altTD;
				IOPhysicalAddress pPhysical;

				
*/

class AppleEHCIIsochEndpoint;

class AppleEHCIQueueHead : public IOUSBControllerListElement
{
    OSDeclareDefaultStructors(AppleEHCIQueueHead)

private:

public:

    // constructor method
    static AppleEHCIQueueHead				*WithSharedMemory(EHCIQueueHeadSharedPtr sharedLogical, IOPhysicalAddress sharedPhysical);

    // virtual methods
    virtual void							SetPhysicalLink(IOPhysicalAddress next);
    virtual IOPhysicalAddress				GetPhysicalLink(void);
    virtual IOPhysicalAddress				GetPhysicalAddrWithType(void);
    virtual void							print(int level);
    
    // not a virtual method, because the return type assumes knowledge of the element type
    EHCIQueueHeadSharedPtr					GetSharedLogical(void);
    
    EHCIGeneralTransferDescriptorPtr		_qTD;
    EHCIGeneralTransferDescriptorPtr		_TailTD;
    UInt16                                  _maxPacketSize;
    UInt8									_direction;
    UInt8									_pollM1;	
    UInt8									_offset;	
    UInt8									_responseToStall;
    UInt8									_queueType;
	UInt8									_bandwidthUsed[8];
};


class AppleEHCIIsochTransferDescriptor : public IOUSBControllerIsochListElement
{
    OSDeclareDefaultStructors(AppleEHCIIsochTransferDescriptor)

public:
    // constructor method
    static AppleEHCIIsochTransferDescriptor 	*WithSharedMemory(EHCIIsochTransferDescriptorSharedPtr sharedLogical, IOPhysicalAddress sharedPhysical);

    // virtual methods
    virtual void					SetPhysicalLink(IOPhysicalAddress next);
    virtual IOPhysicalAddress		GetPhysicalLink(void);
    virtual IOPhysicalAddress		GetPhysicalAddrWithType(void);
    virtual IOReturn				UpdateFrameList(AbsoluteTime timeStamp);
    virtual IOReturn				Deallocate(IOUSBControllerV2 *uim);
    virtual void					print(int level);
    
    // not a virtual method, because the return type assumes knowledge of the element type
    EHCIIsochTransferDescriptorSharedPtr	GetSharedLogical(void);
	
private:
    IOReturn mungeEHCIStatus(UInt32 status, UInt16 *transferLen, UInt32 maxPacketSize, UInt8 direction);
    
};



class AppleEHCISplitIsochTransferDescriptor : public IOUSBControllerIsochListElement
{
    OSDeclareDefaultStructors(AppleEHCISplitIsochTransferDescriptor)

public:
	
    // constructor method
    static AppleEHCISplitIsochTransferDescriptor 	*WithSharedMemory(EHCISplitIsochTransferDescriptorSharedPtr sharedLogical, IOPhysicalAddress sharedPhysical);

    // virtual methods
    virtual void						SetPhysicalLink(IOPhysicalAddress next);
    virtual IOPhysicalAddress			GetPhysicalLink(void);
    virtual IOPhysicalAddress			GetPhysicalAddrWithType(void);
    virtual IOReturn					UpdateFrameList(AbsoluteTime timeStamp);
    virtual IOReturn					Deallocate(IOUSBControllerV2 *uim);
    virtual void						print(int level);

    // not a virtual method, because the return type assumes knowledge of the element type
    EHCISplitIsochTransferDescriptorSharedPtr		GetSharedLogical(void);
    
	// split Isoch specific varibles
	bool								_isDummySITD;
};


class AppleEHCIIsochEndpoint : public IOUSBControllerIsochEndpoint
{
    OSDeclareDefaultStructors(AppleEHCIIsochEndpoint)

public:
	virtual bool	init();
	
	void								*hiPtr;						// pointer to the Transaction Translator (for Split EP)
    short								oneMPS;						// For high bandwidth
    short								mult;						// how many oneMPS sized transactions to do
    USBDeviceAddress					highSpeedHub;
    int									highSpeedPort;
	UInt8								bandwidthUsed[8];			// how many bytes I use on each microframe
	UInt8								startSplitFlags;
	UInt8								completeSplitFlags;
	bool								useBackPtr;
};

#endif
