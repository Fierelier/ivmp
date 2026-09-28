#ifndef IVMP_XLIVE_TYPES_H
#define IVMP_XLIVE_TYPES_H

typedef struct
{
	IN_ADDR ina;
	IN_ADDR inaOnline;
	WORD wPortOnline;
	BYTE abEnet[6];
	BYTE abOnline[20];
} XNADDR;

typedef struct
{
	BYTE ab[8];
} XNKID;

typedef struct
{
	BYTE ab[16];
} XNKEY;

typedef ULONGLONG XUID;
typedef XUID *PXUID;
#define XUSER_NAME_SIZE 16
#define XUSER_INDEX_NONE 0x000000feu
#define XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY 0x00000002u
#define IVMP_IPPROTO_VDP 254
#define XNET_ETHERNET_LINK_ACTIVE 0x00000001u
#define XNET_GET_XNADDR_NONE 0x00000001u
#define XNET_GET_XNADDR_ETHERNET 0x00000002u
#define XNET_GET_XNADDR_STATIC 0x00000004u
#define XNET_GET_XNADDR_GATEWAY 0x00000020u
#define XNET_GET_XNADDR_DNS 0x00000040u
#define XNET_GET_XNADDR_ONLINE 0x00000080u
#define XSESSION_CREATE_HOST 0x00000001u
#define XNET_QOS_LISTEN_SET_DATA 0x00000004u
#define XNET_QOS_LISTEN_RELEASE 0x00000010u
#define XNET_XNQOSINFO_COMPLETE 0x01u
#define XNET_XNQOSINFO_TARGET_CONTACTED 0x02u
#define XNET_XNQOSINFO_TARGET_DISABLED 0x04u
#define XNET_XNQOSINFO_DATA_RECEIVED 0x08u

#define XN_SYS_SIGNINCHANGED 0x0000000Au

typedef enum _XUSER_SIGNIN_STATE {
	eXUserSigninState_NotSignedIn,
	eXUserSigninState_SignedInLocally,
	eXUserSigninState_SignedInToLive
} XUSER_SIGNIN_STATE;

typedef struct _XUSER_SIGNIN_INFO {
	XUID xuid;
	DWORD dwInfoFlags;
	XUSER_SIGNIN_STATE UserSigninState;
	DWORD dwGuestNumber;
	DWORD dwSponsorUserIndex;
	CHAR szUserName[XUSER_NAME_SIZE];
} XUSER_SIGNIN_INFO, *PXUSER_SIGNIN_INFO;

typedef struct _XOVERLAPPED {
	ULONG_PTR InternalLow;
	ULONG_PTR InternalHigh;
	ULONG_PTR InternalContext;
	HANDLE hEvent;
	VOID(WINAPI *pCompletionRoutine)
	(DWORD, DWORD, DWORD);
	DWORD_PTR dwCompletionContext;
	DWORD dwExtendedError;
} XOVERLAPPED, *PXOVERLAPPED;

typedef struct _XUSER_DATA {
	BYTE type;
	union {
		LONG nData;
		LONGLONG i64Data;
		double dblData;
		struct
		{
			DWORD cbData;
			LPWSTR pwszData;
		} string;
		float fData;
		struct
		{
			DWORD cbData;
			LPBYTE pbData;
		} binary;
		FILETIME ftData;
	};
} XUSER_DATA, *PXUSER_DATA;

typedef enum _XUSER_PROFILE_SOURCE {
	XSOURCE_NO_VALUE = 0,
	XSOURCE_DEFAULT,
	XSOURCE_TITLE,
	XSOURCE_PERMISSION_DENIED
} XUSER_PROFILE_SOURCE;

typedef struct _XUSER_PROFILE_SETTING {
	XUSER_PROFILE_SOURCE source;
	union {
		DWORD dwUserIndex;
		XUID xuid;
	} user;
	DWORD dwSettingId;
	XUSER_DATA data;
} XUSER_PROFILE_SETTING, *PXUSER_PROFILE_SETTING;

typedef struct _XUSER_READ_PROFILE_SETTING_RESULT {
	DWORD dwSettingsLen;
	XUSER_PROFILE_SETTING *pSettings;
} XUSER_READ_PROFILE_SETTING_RESULT, *PXUSER_READ_PROFILE_SETTING_RESULT;

typedef struct _XCONTENT_DATA {
	DWORD ContentNum;
	DWORD TitleId;
	DWORD ContentPackageType;
	BYTE ContentId[20];
} XCONTENT_DATA, *PXCONTENT_DATA;

typedef struct _XUSER_ACHIEVEMENT {
	DWORD dwUserIndex;
	DWORD dwAchievementId;
} XUSER_ACHIEVEMENT, *PXUSER_ACHIEVEMENT;

typedef struct
{
	XNKID sessionID;
	XNADDR hostAddress;
	XNKEY keyExchangeKey;
} XSESSION_INFO, *PXSESSION_INFO;

typedef enum _XSESSION_STATE {
	XSESSION_STATE_LOBBY = 0,
	XSESSION_STATE_REGISTRATION,
	XSESSION_STATE_INGAME,
	XSESSION_STATE_REPORTING,
	XSESSION_STATE_DELETED
} XSESSION_STATE;

typedef struct
{
	XUID xuidOnline;
	DWORD dwUserIndex;
	DWORD dwFlags;
} XSESSION_MEMBER;

typedef struct
{
	DWORD dwUserIndexHost;
	DWORD dwGameType;
	DWORD dwGameMode;
	DWORD dwFlags;
	DWORD dwMaxPublicSlots;
	DWORD dwMaxPrivateSlots;
	DWORD dwAvailablePublicSlots;
	DWORD dwAvailablePrivateSlots;
	DWORD dwActualMemberCount;
	DWORD dwReturnedMemberCount;
	XSESSION_STATE eState;
	ULONGLONG qwNonce;
	XSESSION_INFO sessionInfo;
	XNKID xnkidArbitration;
	XSESSION_MEMBER *pSessionMembers;
} XSESSION_LOCAL_DETAILS, *PXSESSION_LOCAL_DETAILS;

typedef struct _XUSER_CONTEXT {
	DWORD dwContextId;
	DWORD dwValue;
} XUSER_CONTEXT, *PXUSER_CONTEXT;

typedef struct _XUSER_PROPERTY {
	DWORD dwPropertyId;
	XUSER_DATA value;
} XUSER_PROPERTY, *PXUSER_PROPERTY;

typedef struct _XSESSION_SEARCHRESULT {
	XSESSION_INFO info;
	DWORD dwOpenPublicSlots;
	DWORD dwOpenPrivateSlots;
	DWORD dwFilledPublicSlots;
	DWORD dwFilledPrivateSlots;
	DWORD cProperties;
	DWORD cContexts;
	PXUSER_PROPERTY pProperties;
	PXUSER_CONTEXT pContexts;
} XSESSION_SEARCHRESULT, *PXSESSION_SEARCHRESULT;

typedef struct _XSESSION_SEARCHRESULT_HEADER {
	DWORD dwSearchResults;
	XSESSION_SEARCHRESULT *pResults;
} XSESSION_SEARCHRESULT_HEADER, *PXSESSION_SEARCHRESULT_HEADER;

typedef struct
{
	BYTE bFlags;
	BYTE bReserved;
	WORD cProbesXmit;
	WORD cProbesRecv;
	WORD cbData;
	BYTE *pbData;
	WORD wRttMinInMsecs;
	WORD wRttMedInMsecs;
	DWORD dwUpBitsPerSec;
	DWORD dwDnBitsPerSec;
} XNQOSINFO;

typedef struct
{
	UINT cxnqos;
	UINT cxnqosPending;
	XNQOSINFO axnqosinfo[1];
} XNQOS;

typedef enum {
	XONLINE_NAT_OPEN = 1,
	XONLINE_NAT_MODERATE,
	XONLINE_NAT_STRICT
} XONLINE_NAT_TYPE;

typedef struct _STRING_DATA {
	WORD wStringSize;
	WCHAR *pszString;
} STRING_DATA;

#pragma pack(push, 1)
typedef struct _STRING_VERIFY_RESPONSE {
	WORD wNumStrings;
	HRESULT *pStringResult;
} STRING_VERIFY_RESPONSE;
#pragma pack(pop)

typedef enum _XSTORAGE_FACILITY {
	XSTORAGE_FACILITY_GAME_CLIP = 1,
	XSTORAGE_FACILITY_PER_TITLE = 2,
	XSTORAGE_FACILITY_PER_USER_TITLE = 3
} XSTORAGE_FACILITY;

typedef struct _XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS {
	DWORD dwBytesTotal;
	XUID xuidOwner;
	FILETIME ftCreated;
} XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS;

typedef struct
{
	DWORD dwNewOffers;
	DWORD dwTotalOffers;
} XOFFERING_CONTENTAVAILABLE_RESULT;

typedef struct
{
	HANDLE id;
	DWORD dwSize;
	DWORD magic;
	LPBYTE pbData;
} ivmp_xlive_pbuffer;

#endif
