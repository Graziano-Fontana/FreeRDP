/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * NTLM over HTTP
 *
 * Copyright 2012 Fujitsu Technology Solutions GmbH
 * Copyright 2012 Dmitrij Jasnov <dmitrij.jasnov@ts.fujitsu.com>
 * Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/tchar.h>
#include <winpr/dsparse.h>

#include <freerdp/log.h>

#include "http.h"

#include "ntlm.h"

#define TAG FREERDP_TAG("core.gateway.ntlm")

struct rdp_ntlm
{
	BOOL http;
	CtxtHandle context;
	ULONG cbMaxToken;
	ULONG fContextReq;
	ULONG pfContextAttr;
	TimeStamp expiration;
	PSecBuffer pBuffer;
	SecBuffer inputBuffer[2];
	SecBuffer outputBuffer[2];
	BOOL haveContext;
	BOOL haveInputBuffer;
	LPTSTR ServicePrincipalName;
	SecBufferDesc inputBufferDesc;
	SecBufferDesc outputBufferDesc;
	CredHandle credentials;
	BOOL confidentiality;
	SecPkgInfo* pPackageInfo;
	SecurityFunctionTable* table;
	SEC_WINNT_AUTH_IDENTITY identity;
	SecPkgContext_Sizes ContextSizes;
	SecPkgContext_Bindings* Bindings;
};

static ULONG cast_from_size_(size_t size, const char* fkt, const char* file, int line)
{
	if (size > ULONG_MAX)
	{
		WLog_ERR(TAG, "[%s %s:%d] Size %" PRIdz " is larger than INT_MAX %lu", fkt, file, line,
		         size, ULONG_MAX);
		return 0;
	}

	return (ULONG)size;
}

#define cast_from_size(size) cast_from_size_(size, __FUNCTION__, __FILE__, __LINE__)

BOOL ntlm_client_init(rdpNtlm* ntlm, BOOL http, LPCSTR user, LPCSTR domain, LPCSTR password,
                      SecPkgContext_Bindings* Bindings)
{
	SECURITY_STATUS status;
	ntlm->http = http;
	ntlm->Bindings = Bindings;
	ntlm->table = InitSecurityInterfaceEx(0);

	if (!ntlm->table)
		return FALSE;

	sspi_SetAuthIdentity(&(ntlm->identity), user, domain, password);
	status = ntlm->table->QuerySecurityPackageInfo(NTLM_SSP_NAME, &ntlm->pPackageInfo);

	if (status != SEC_E_OK)
	{
		WLog_ERR(TAG, "QuerySecurityPackageInfo status %s [0x%08" PRIX32 "]",
		         GetSecurityStatusString(status), status);
		return FALSE;
	}

	ntlm->cbMaxToken = ntlm->pPackageInfo->cbMaxToken;
	status = ntlm->table->AcquireCredentialsHandle(NULL, NTLM_SSP_NAME, SECPKG_CRED_OUTBOUND, NULL,
	                                               &ntlm->identity, NULL, NULL, &ntlm->credentials,
	                                               &ntlm->expiration);

	if (status != SEC_E_OK)
	{
		WLog_ERR(TAG, "AcquireCredentialsHandle status %s [0x%08" PRIX32 "]",
		         GetSecurityStatusString(status), status);
		return FALSE;
	}

	ntlm->haveContext = FALSE;
	ntlm->haveInputBuffer = FALSE;
	ZeroMemory(&ntlm->inputBuffer, sizeof(SecBuffer));
	ZeroMemory(&ntlm->outputBuffer, sizeof(SecBuffer));
	ZeroMemory(&ntlm->ContextSizes, sizeof(SecPkgContext_Sizes));
	ntlm->fContextReq = 0;

	if (ntlm->http)
	{
		/* flags for HTTP authentication */
		ntlm->fContextReq |= ISC_REQ_CONFIDENTIALITY;
	}
	else
	{
		/**
		 * flags for RPC authentication:
		 * RPC_C_AUTHN_LEVEL_PKT_INTEGRITY:
		 * ISC_REQ_USE_DCE_STYLE | ISC_REQ_DELEGATE | ISC_REQ_MUTUAL_AUTH |
		 * ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT
		 */
		ntlm->fContextReq |= ISC_REQ_USE_DCE_STYLE;
		ntlm->fContextReq |= ISC_REQ_DELEGATE | ISC_REQ_MUTUAL_AUTH;
		ntlm->fContextReq |= ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT;
	}

	return TRUE;
}

BOOL ntlm_client_make_spn(rdpNtlm* ntlm, LPCSTR ServiceClass, LPCSTR hostname)
{
	BOOL status = FALSE;
	DWORD SpnLength = 0;
#ifdef UNICODE
	LPWSTR hostnameX = NULL;
	ConvertToUnicode(CP_UTF8, 0, hostname, -1, (LPWSTR*)&hostnameX, 0);
#else
	LPSTR hostnameX = _strdup(hostname);
#endif

	if (!hostnameX)
		return FALSE;

	if (!ServiceClass)
	{
		ntlm->ServicePrincipalName = hostnameX;
		return TRUE;
	}

	if (DsMakeSpn(ServiceClass, hostnameX, NULL, 0, hostnameX, &SpnLength, NULL) !=
	    ERROR_BUFFER_OVERFLOW)
		goto error;

	ntlm->ServicePrincipalName = (LPTSTR)calloc(SpnLength, sizeof(TCHAR));

	if (!ntlm->ServicePrincipalName)
		goto error;

	if (DsMakeSpn(ServiceClass, hostnameX, NULL, 0, hostnameX, &SpnLength,
	              ntlm->ServicePrincipalName) != ERROR_SUCCESS)
		goto error;

	status = TRUE;
error:
	free(hostnameX);
	return status;
}

/**
 *                                        SSPI Client Ceremony
 *
 *                                           --------------
 *                                          ( Client Begin )
 *                                           --------------
 *                                                 |
 *                                                 |
 *                                                \|/
 *                                      -----------+--------------
 *                                     | AcquireCredentialsHandle |
 *                                      --------------------------
 *                                                 |
 *                                                 |
 *                                                \|/
 *                                    -------------+--------------
 *                 +---------------> / InitializeSecurityContext /
 *                 |                 ----------------------------
 *                 |                               |
 *                 |                               |
 *                 |                              \|/
 *     ---------------------------        ---------+-------------            ----------------------
 *    / Receive blob from server /      < Received security blob? > --Yes-> / Send blob to server /
 *    -------------+-------------         -----------------------           ----------------------
 *                /|\                              |                                |
 *                 |                               No                               |
 *                Yes                             \|/                               |
 *                 |                   ------------+-----------                     |
 *                 +---------------- < Received Continue Needed > <-----------------+
 *                                     ------------------------
 *                                                 |
 *                                                 No
 *                                                \|/
 *                                           ------+-------
 *                                          (  Client End  )
 *                                           --------------
 */

BOOL ntlm_authenticate(rdpNtlm* ntlm, BOOL* pbContinueNeeded)
{
	SECURITY_STATUS status;

	if ((!ntlm) || (!ntlm->table))
	{
		WLog_ERR(TAG, "ntlm_authenticate: invalid ntlm context");
		return FALSE;
	}

	if (ntlm->outputBuffer[0].pvBuffer)
	{
		free(ntlm->outputBuffer[0].pvBuffer);
		ntlm->outputBuffer[0].pvBuffer = NULL;
	}

	ntlm->outputBufferDesc.ulVersion = SECBUFFER_VERSION;
	ntlm->outputBufferDesc.cBuffers = 1;
	ntlm->outputBufferDesc.pBuffers = ntlm->outputBuffer;
	ntlm->outputBuffer[0].BufferType = SECBUFFER_TOKEN;
	ntlm->outputBuffer[0].cbBuffer = ntlm->cbMaxToken;
	ntlm->outputBuffer[0].pvBuffer = malloc(ntlm->outputBuffer[0].cbBuffer);

	if (!ntlm->outputBuffer[0].pvBuffer)
		return FALSE;

	if (ntlm->haveInputBuffer)
	{
		ntlm->inputBufferDesc.ulVersion = SECBUFFER_VERSION;
		ntlm->inputBufferDesc.cBuffers = 1;
		ntlm->inputBufferDesc.pBuffers = ntlm->inputBuffer;
		ntlm->inputBuffer[0].BufferType = SECBUFFER_TOKEN;

		if (ntlm->Bindings)
		{
			ntlm->inputBufferDesc.cBuffers++;
			ntlm->inputBuffer[1].BufferType = SECBUFFER_CHANNEL_BINDINGS;
			ntlm->inputBuffer[1].cbBuffer = ntlm->Bindings->BindingsLength;
			ntlm->inputBuffer[1].pvBuffer = (void*)ntlm->Bindings->Bindings;
		}
	}

	status = ntlm->table->InitializeSecurityContext(
	    &ntlm->credentials, (ntlm->haveContext) ? &ntlm->context : NULL,
	    (ntlm->ServicePrincipalName) ? ntlm->ServicePrincipalName : NULL, ntlm->fContextReq, 0,
	    SECURITY_NATIVE_DREP, (ntlm->haveInputBuffer) ? &ntlm->inputBufferDesc : NULL, 0,
	    &ntlm->context, &ntlm->outputBufferDesc, &ntlm->pfContextAttr, &ntlm->expiration);
	WLog_VRB(TAG, "InitializeSecurityContext status %s [0x%08" PRIX32 "]",
	         GetSecurityStatusString(status), status);

	if ((status == SEC_I_COMPLETE_AND_CONTINUE) || (status == SEC_I_COMPLETE_NEEDED) ||
	    (status == SEC_E_OK))
	{
		if ((status != SEC_E_OK) && ntlm->table->CompleteAuthToken)
		{
			SECURITY_STATUS cStatus;
			cStatus = ntlm->table->CompleteAuthToken(&ntlm->context, &ntlm->outputBufferDesc);

			if (cStatus != SEC_E_OK)
			{
				WLog_WARN(TAG, "CompleteAuthToken status  %s [0x%08" PRIX32 "]",
				          GetSecurityStatusString(cStatus), cStatus);
				return FALSE;
			}
		}

		status = ntlm->table->QueryContextAttributes(&ntlm->context, SECPKG_ATTR_SIZES,
		                                             &ntlm->ContextSizes);

		if (status != SEC_E_OK)
		{
			WLog_ERR(TAG, "QueryContextAttributes SECPKG_ATTR_SIZES failure %s [0x%08" PRIX32 "]",
			         GetSecurityStatusString(status), status);
			return FALSE;
		}

		if (status == SEC_I_COMPLETE_NEEDED)
			status = SEC_E_OK;
		else if (status == SEC_I_COMPLETE_AND_CONTINUE)
			status = SEC_I_CONTINUE_NEEDED;
	}

	if (ntlm->haveInputBuffer)
	{
		free(ntlm->inputBuffer[0].pvBuffer);
		ntlm->inputBuffer[0].pvBuffer = NULL;
	}

	ntlm->haveInputBuffer = TRUE;
	ntlm->haveContext = TRUE;

	if (pbContinueNeeded)
		*pbContinueNeeded = (status == SEC_I_CONTINUE_NEEDED) ? TRUE : FALSE;

	return TRUE;
}

static void ntlm_client_uninit(rdpNtlm* ntlm)
{
	free(ntlm->identity.User);
	ntlm->identity.User = NULL;
	free(ntlm->identity.Domain);
	ntlm->identity.Domain = NULL;
	free(ntlm->identity.Password);
	ntlm->identity.Password = NULL;
	free(ntlm->ServicePrincipalName);
	ntlm->ServicePrincipalName = NULL;

	if (ntlm->table)
	{
		SECURITY_STATUS status;
		status = ntlm->table->FreeCredentialsHandle(&ntlm->credentials);

		if (status != SEC_E_OK)
		{
			WLog_WARN(TAG, "FreeCredentialsHandle status %s [0x%08" PRIX32 "]",
			          GetSecurityStatusString(status), status);
		}

		status = ntlm->table->FreeContextBuffer(ntlm->pPackageInfo);

		if (status != SEC_E_OK)
		{
			WLog_WARN(TAG, "FreeContextBuffer status %s [0x%08" PRIX32 "]",
			          GetSecurityStatusString(status), status);
		}

		status = ntlm->table->DeleteSecurityContext(&ntlm->context);

		if (status != SEC_E_OK)
		{
			WLog_WARN(TAG, "DeleteSecurityContext status %s [0x%08" PRIX32 "]",
			          GetSecurityStatusString(status), status);
		}

		ntlm->table = NULL;
	}
}

rdpNtlm* ntlm_new(void)
{
	rdpNtlm* ntlm;
	ntlm = (rdpNtlm*)calloc(1, sizeof(rdpNtlm));
	return ntlm;
}

void ntlm_free(rdpNtlm* ntlm)
{
	if (!ntlm)
		return;

	if (ntlm->outputBuffer[0].pvBuffer)
	{
		free(ntlm->outputBuffer[0].pvBuffer);
		ntlm->outputBuffer[0].pvBuffer = NULL;
	}

	ntlm_client_uninit(ntlm);
	free(ntlm);
}

SSIZE_T ntlm_client_get_context_max_size(rdpNtlm* ntlm)
{
	if (!ntlm)
		return -1;

	if (ntlm->ContextSizes.cbMaxSignature > UINT16_MAX)
	{
		WLog_ERR(TAG,
		         "QueryContextAttributes SECPKG_ATTR_SIZES ContextSizes.cbMaxSignature > 0xFFFF");
		return -1;
	}

	return ntlm->ContextSizes.cbMaxSignature;
}

SSIZE_T ntlm_client_query_auth_size(rdpNtlm* ntlm)
{
	SECURITY_STATUS status;

	if (!ntlm || !ntlm->table || !ntlm->table->QueryContextAttributes)
		return -1;

	status =
	    ntlm->table->QueryContextAttributes(&ntlm->context, SECPKG_ATTR_SIZES, &ntlm->ContextSizes);

	if (status != SEC_E_OK)
	{
		WLog_ERR(TAG, "QueryContextAttributes SECPKG_ATTR_SIZES failure %s [0x%08" PRIX32 "]",
		         GetSecurityStatusString(status), status);
		return -1;
	}

	return ntlm_client_get_context_max_size(ntlm);
}

BOOL ntlm_client_encrypt(rdpNtlm* ntlm, ULONG fQOP, SecBufferDesc* Message, size_t sequence)
{
	SECURITY_STATUS encrypt_status;
	const ULONG s = cast_from_size(sequence);

	if (!ntlm || !Message)
		return FALSE;

	encrypt_status = ntlm->table->EncryptMessage(&ntlm->context, fQOP, Message, s);

	if (encrypt_status != SEC_E_OK)
	{
		WLog_ERR(TAG, "EncryptMessage status %s [0x%08" PRIX32 "]",
		         GetSecurityStatusString(encrypt_status), encrypt_status);
		return FALSE;
	}

	return TRUE;
}

BOOL ntlm_client_set_input_buffer(rdpNtlm* ntlm, BOOL copy, const void* data, size_t size)
{
	if (!ntlm || !data || (size == 0))
		return FALSE;

	ntlm->inputBuffer[0].cbBuffer = cast_from_size(size);

	if (copy)
	{
		ntlm->inputBuffer[0].pvBuffer = malloc(size);

		if (!ntlm->inputBuffer[0].pvBuffer)
			return FALSE;

		memcpy(ntlm->inputBuffer[0].pvBuffer, data, size);
	}
	else
		ntlm->inputBuffer[0].pvBuffer = (void*)data;

	return TRUE;
}

const SecBuffer* ntlm_client_get_output_buffer(rdpNtlm* ntlm)
{
	if (!ntlm)
		return NULL;

	return &ntlm->outputBuffer[0];
}
