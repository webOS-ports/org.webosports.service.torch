// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/*
 * torchd - com.webos.service.torch
 *
 * Puts the torch on the bus. Everything device-specific lives one layer down in
 * nyx: this opens NYX_DEVICE_LED "Torch" and neither knows nor cares whether
 * that resolves to the sysfs module or the hybris one that asks the Android
 * camera service. The two disagree about brightness - the camera service has no
 * intensity, only on/off - which is why the API leads with a boolean and treats
 * brightness as the optional refinement.
 */

#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <nyx/nyx_client.h>
#include <pbnjson.h>
#include <stdbool.h>
#include <stdlib.h>

#define TORCH_SERVICE "com.webos.service.torch"

static LSHandle *service_handle = NULL;
static nyx_device_handle_t torch_device = NULL;
static bool nyx_attempted = false;

/*
 * Opened on demand rather than at start-up. torchd is socket-activated and a
 * device with no torch is a normal state, not a failure to boot: opening lazily
 * lets getStatus answer "available: false" instead of the service dying and
 * taking the answer with it.
 */
static bool ensure_torch(void)
{
	nyx_error_t err;

	if (torch_device)
	{
		return true;
	}

	if (nyx_attempted)
	{
		return false;
	}

	nyx_attempted = true;
	err = nyx_device_open(NYX_DEVICE_LED, "Torch", &torch_device);

	if (err != NYX_ERROR_NONE || torch_device == NULL)
	{
		g_warning("no torch device: nyx error %d", err);
		torch_device = NULL;
		return false;
	}

	return true;
}

static int current_brightness(void)
{
	int32_t brightness = 0;

	if (!ensure_torch())
	{
		return 0;
	}

	if (nyx_led_get_brightness(torch_device, &brightness) != NYX_ERROR_NONE)
	{
		return 0;
	}

	return (int)brightness;
}

static jvalue_ref build_status(void)
{
	bool available = ensure_torch();
	int brightness = available ? current_brightness() : 0;

	jvalue_ref reply = jobject_create();
	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"), jboolean_create(available));
	jobject_put(reply, J_CSTR_TO_JVAL("on"), jboolean_create(brightness > 0));
	jobject_put(reply, J_CSTR_TO_JVAL("brightness"), jnumber_create_i32(brightness));
	return reply;
}

static void post_status(void)
{
	LSError lserror;
	jvalue_ref status = build_status();
	const char *payload = jvalue_tostring_simple(status);

	LSErrorInit(&lserror);

	if (!LSSubscriptionReply(service_handle, "/getStatus", payload, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&status);
}

static bool reply_error(LSHandle *sh, LSMessage *message, const char *text)
{
	LSError lserror;
	jvalue_ref reply = jobject_create();
	bool ok;

	LSErrorInit(&lserror);
	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(text));

	ok = LSMessageReply(sh, message, jvalue_tostring_simple(reply), &lserror);

	if (!ok)
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&reply);
	return true;
}

static bool reply_status(LSHandle *sh, LSMessage *message)
{
	LSError lserror;
	jvalue_ref status = build_status();

	LSErrorInit(&lserror);

	if (!LSMessageReply(sh, message, jvalue_tostring_simple(status), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&status);
	return true;
}

static bool cb_get_status(LSHandle *sh, LSMessage *message, void *ctx)
{
	LSError lserror;
	LSErrorInit(&lserror);

	if (LSMessageIsSubscription(message) &&
	        !LSSubscriptionProcess(sh, message, &(bool){false}, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	return reply_status(sh, message);
}

/*
 * set accepts {"on": bool} or {"brightness": 0-100}, and brightness wins when
 * both are given - it is the more specific request. A brightness the backend
 * cannot honour is not an error: the hybris backend has only on and off, and
 * saturating there is better than refusing a request the user can see should
 * have worked.
 */
static bool cb_set(LSHandle *sh, LSMessage *message, void *ctx)
{
	JSchemaInfo schema;
	jvalue_ref parsed, value;
	int32_t brightness = -1;
	bool on = false;
	nyx_error_t err;

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)),
	                    DOMOPT_NOOPT, &schema);

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, message, "malformed json");
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("brightness"), &value) &&
	        jis_number(value))
	{
		jnumber_get_i32(value, &brightness);
	}
	else if (jobject_get_exists(parsed, J_CSTR_TO_BUF("on"), &value) &&
	         jis_boolean(value))
	{
		jboolean_get(value, &on);
		brightness = on ? 100 : 0;
	}

	j_release(&parsed);

	if (brightness < 0 || brightness > 100)
	{
		return reply_error(sh, message,
		                   "need \"on\": boolean, or \"brightness\": 0-100");
	}

	if (!ensure_torch())
	{
		return reply_error(sh, message, "no torch on this device");
	}

	err = nyx_led_set_brightness(torch_device, brightness);

	if (err != NYX_ERROR_NONE)
	{
		return reply_error(sh, message, "the torch device rejected the request");
	}

	post_status();
	return reply_status(sh, message);
}

static bool cb_toggle(LSHandle *sh, LSMessage *message, void *ctx)
{
	nyx_error_t err;

	if (!ensure_torch())
	{
		return reply_error(sh, message, "no torch on this device");
	}

	err = nyx_led_set_brightness(torch_device, current_brightness() > 0 ? 0 : 100);

	if (err != NYX_ERROR_NONE)
	{
		return reply_error(sh, message, "the torch device rejected the request");
	}

	post_status();
	return reply_status(sh, message);
}

static LSMethod methods[] =
{
	{ "getStatus", cb_get_status, LUNA_METHOD_FLAGS_NONE },
	{ "set",       cb_set,        LUNA_METHOD_FLAGS_NONE },
	{ "toggle",    cb_toggle,     LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

int main(int argc, char **argv)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	LSError lserror;

	LSErrorInit(&lserror);

	if (!LSRegister(TORCH_SERVICE, &service_handle, &lserror))
	{
		goto fail;
	}

	if (!LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror))
	{
		goto fail;
	}

	if (!LSGmainAttach(service_handle, loop, &lserror))
	{
		goto fail;
	}

	g_main_loop_run(loop);

	/*
	 * Only reached on an orderly shutdown. Closing the nyx handle matters:
	 * the hybris backend turns the torch off there, so a stop does not leave
	 * the light on with nothing left able to switch it off.
	 */
	if (torch_device)
	{
		nyx_device_close(torch_device);
	}

	LSUnregister(service_handle, &lserror);
	g_main_loop_unref(loop);
	return 0;

fail:
	LSErrorPrint(&lserror, stderr);
	LSErrorFree(&lserror);
	g_main_loop_unref(loop);
	return 1;
}
