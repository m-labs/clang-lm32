// RUN: %clang_cc1 -analyze -analyzer-checker=experimental.osx.KeychainAPI %s -verify

// Fake typedefs.
typedef unsigned int OSStatus;
typedef unsigned int SecKeychainAttributeList;
typedef unsigned int SecKeychainItemRef;
typedef unsigned int SecItemClass;
typedef unsigned int UInt32;
typedef unsigned int CFTypeRef;
typedef unsigned int UInt16;
typedef unsigned int SecProtocolType;
typedef unsigned int SecAuthenticationType;
typedef unsigned int SecKeychainAttributeInfo;
enum {
  noErr                      = 0,
  GenericError               = 1
};

// Functions that allocate data.
OSStatus SecKeychainItemCopyContent (
    SecKeychainItemRef itemRef,
    SecItemClass *itemClass,
    SecKeychainAttributeList *attrList,
    UInt32 *length,
    void **outData
);
OSStatus SecKeychainFindGenericPassword (
    CFTypeRef keychainOrArray,
    UInt32 serviceNameLength,
    const char *serviceName,
    UInt32 accountNameLength,
    const char *accountName,
    UInt32 *passwordLength,
    void **passwordData,
    SecKeychainItemRef *itemRef
);
OSStatus SecKeychainFindInternetPassword (
    CFTypeRef keychainOrArray,
    UInt32 serverNameLength,
    const char *serverName,
    UInt32 securityDomainLength,
    const char *securityDomain,
    UInt32 accountNameLength,
    const char *accountName,
    UInt32 pathLength,
    const char *path,
    UInt16 port,
    SecProtocolType protocol,
    SecAuthenticationType authenticationType,
    UInt32 *passwordLength,
    void **passwordData,
    SecKeychainItemRef *itemRef
);
OSStatus SecKeychainItemCopyAttributesAndData (
   SecKeychainItemRef itemRef,
   SecKeychainAttributeInfo *info,
   SecItemClass *itemClass,
   SecKeychainAttributeList **attrList,
   UInt32 *length,
   void **outData
);

// Functions which free data.
OSStatus SecKeychainItemFreeContent (
    SecKeychainAttributeList *attrList,
    void *data
);
OSStatus SecKeychainItemFreeAttributesAndData (
   SecKeychainAttributeList *attrList,
   void *data
);

void errRetVal() {
	unsigned int *ptr = 0;
	OSStatus st = 0;
	UInt32 length;
	void *outData;
	st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData);
	if (st == GenericError) // expected-warning{{Allocated data is not released: missing a call to 'SecKeychainItemFreeContent'.}}
		SecKeychainItemFreeContent(ptr, outData); // expected-warning{{Trying to free data which has not been allocated.}}
}

// If null is passed in, the data is not allocated, so no need for the matching free.
void fooDoNotReportNull() {
    unsigned int *ptr = 0;
    OSStatus st = 0;
    UInt32 *length = 0;
    void **outData = 0;
    SecKeychainItemCopyContent(2, ptr, ptr, 0, 0);
    SecKeychainItemCopyContent(2, ptr, ptr, length, outData);
}// no-warning

void doubleAlloc() {
    unsigned int *ptr = 0;
    OSStatus st = 0;
    UInt32 length;
    void *outData;
    st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData);
    st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData); // expected-warning {{Allocated data should be released before another call to the allocator:}}
    if (st == noErr)
      SecKeychainItemFreeContent(ptr, outData);
}

void fooOnlyFree() {
  unsigned int *ptr = 0;
  OSStatus st = 0;
  UInt32 length;
  void *outData = &length;
  SecKeychainItemFreeContent(ptr, outData);// expected-warning{{Trying to free data which has not been allocated}}
}

// Do not warn if undefined value is passed to a function.
void fooOnlyFreeUndef() {
  unsigned int *ptr = 0;
  OSStatus st = 0;
  UInt32 length;
  void *outData;
  SecKeychainItemFreeContent(ptr, outData);
}// no-warning

// Do not warn if the address is a parameter in the enclosing function.
void fooOnlyFreeParam(void *attrList, void* X) {
    SecKeychainItemFreeContent(attrList, X); 
}// no-warning

// If we are returning the value, no not report.
void* returnContent() {
  unsigned int *ptr = 0;
  OSStatus st = 0;
  UInt32 length;
  void *outData;
  st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData);
  return outData;
} // no-warning

int apiMismatch(SecKeychainItemRef itemRef, 
         SecKeychainAttributeInfo *info,
         SecItemClass *itemClass) {
  OSStatus st = 0;
  SecKeychainAttributeList *attrList;
  UInt32 length;
  void *outData;
  
  st = SecKeychainItemCopyAttributesAndData(itemRef, info, itemClass, 
                                            &attrList, &length, &outData); 
  if (st == noErr)
    SecKeychainItemFreeContent(attrList, outData); // expected-warning{{Allocator doesn't match the deallocator}}
  return 0;
}

int ErrorCodesFromDifferentAPISDoNotInterfere(SecKeychainItemRef itemRef, 
                                              SecKeychainAttributeInfo *info,
                                              SecItemClass *itemClass) {
  unsigned int *ptr = 0;
  OSStatus st = 0;
  UInt32 length;
  void *outData;
  OSStatus st2 = 0;
  SecKeychainAttributeList *attrList;
  UInt32 length2;
  void *outData2;

  st2 = SecKeychainItemCopyAttributesAndData(itemRef, info, itemClass, 
                                             &attrList, &length2, &outData2);
  st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData);  
  if (st == noErr) {
    SecKeychainItemFreeContent(ptr, outData);
    if (st2 == noErr) {
      SecKeychainItemFreeAttributesAndData(attrList, outData2);
    }
  } 
  return 0; // expected-warning{{Allocated data is not released: missing a call to 'SecKeychainItemFreeAttributesAndData'}}
}

int foo() {
  unsigned int *ptr = 0;
  OSStatus st = 0;

  UInt32 length;
  void *outData;

  st = SecKeychainItemCopyContent(2, ptr, ptr, &length, &outData);
  if (st == noErr)
    SecKeychainItemFreeContent(ptr, outData);

  return 0;
}// no-warning
