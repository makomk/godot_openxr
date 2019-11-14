////////////////////////////////////////////////////////////////////////////////////////////////
// Helper calls and singleton container for accessing openxr

#include "OXRCalls.h"
#include <stdint.h>

openxr_data_struct* openxr_data_singleton = NULL;

void
openxr_release_data()
{
	if (openxr_data_singleton == NULL) {
		// nothing to release
		printf("OpenXR: tried to release non-existant OpenXR context\n");
	} else if (openxr_data_singleton->use_count > 1) {
		// decrease use count
		openxr_data_singleton->use_count--;
		printf("OpenXR: decreased use count to %i\n",
		       openxr_data_singleton->use_count);
	} else {
		// cleanup openxr
		printf("OpenXR: releasing OpenXR context\n");

		deinit_openxr(openxr_data_singleton->api);

		api->godot_free(openxr_data_singleton);
		openxr_data_singleton = NULL;
	};
};

openxr_data_struct*
openxr_get_data()
{
	if (openxr_data_singleton != NULL) {
		// increase use count
		openxr_data_singleton->use_count++;
		printf("OpenXR: increased use count to %i\n",
		       openxr_data_singleton->use_count);
	} else {
		// init openxr
		printf("OpenXR: initialising OpenXR context\n");

		openxr_data_singleton =
		    (openxr_data_struct*)api->godot_alloc(sizeof(openxr_data_struct));
		if (openxr_data_singleton != NULL) {
			openxr_data_singleton->api = init_openxr();
			if (openxr_data_singleton->api == NULL) {
				printf("OpenXR init failed\n");
				api->godot_free(openxr_data_singleton);
				openxr_data_singleton = NULL;
			} else {
				printf("OpenXR init succeeded\n");
			}
		};
	}

	return openxr_data_singleton;
};



#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>

#define GL_GLEXT_PROTOTYPES 1
#define GL3_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <X11/Xlib.h>
#include <GL/glx.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

typedef struct xr_api
{
	XrInstance instance;
	XrSession session;
	XrSpace local_space;
	XrGraphicsBindingOpenGLXlibKHR graphics_binding_gl;
	XrSwapchainImageOpenGLKHR** images;
	XrSwapchain* swapchains;
	uint32_t view_count;
	XrViewConfigurationView* configuration_views;
	// GLuint** framebuffers;
	// GLuint depthbuffer;

	XrCompositionLayerProjection* projectionLayer;
	XrFrameState* frameState;
	bool running;
	bool visible;

	XrView* views;
	XrCompositionLayerProjectionView* projection_views;
} xr_api;

bool
xr_result(XrInstance instance, XrResult result, const char* format, ...)
{
	if (XR_SUCCEEDED(result))
		return true;

	char resultString[XR_MAX_RESULT_STRING_SIZE];
	xrResultToString(instance, result, resultString);

	size_t len1 = strlen(format);
	size_t len2 = strlen(resultString) + 1;
	char formatRes[len1 + len2 + 4]; // + " []\n"
	sprintf(formatRes, "%s [%s]\n", format, resultString);

	va_list args;
	va_start(args, format);
	vprintf(formatRes, args);
	va_end(args);
	return false;
}

bool
isExtensionSupported(char* extensionName,
                     XrExtensionProperties* instanceExtensionProperties,
                     uint32_t instanceExtensionCount)
{
	for (uint32_t supportedIndex = 0; supportedIndex < instanceExtensionCount;
	     supportedIndex++) {
		if (!strcmp(extensionName,
		            instanceExtensionProperties[supportedIndex].extensionName)) {
			return true;
		}
	}
	return false;
}

bool
isViewConfigSupported(XrInstance instance,
                      XrViewConfigurationType type,
                      XrSystemId systemId)
{
	XrResult result;
	uint32_t viewConfigurationCount;
	result = xrEnumerateViewConfigurations(instance, systemId, 0,
	                                       &viewConfigurationCount, NULL);
	if (!xr_result(instance, result, "Failed to get view configuration count"))
		return false;
	XrViewConfigurationType viewConfigurations[viewConfigurationCount];
	result = xrEnumerateViewConfigurations(
	    instance, systemId, viewConfigurationCount, &viewConfigurationCount,
	    viewConfigurations);
	if (!xr_result(instance, result, "Failed to enumerate view configurations!"))
		return 1;

	for (uint32_t i = 0; i < viewConfigurationCount; ++i) {

		if (viewConfigurations[i] == type)
			return true;
	}
	return false;
}

bool
isReferenceSpaceSupported(XrInstance instance,
                          XrSession session,
                          XrReferenceSpaceType type)
{
	XrResult result;
	uint32_t referenceSpacesCount;
	result = xrEnumerateReferenceSpaces(session, 0, &referenceSpacesCount, NULL);
	if (!xr_result(instance, result,
	               "Getting number of reference spaces failed!"))
		return 1;

	XrReferenceSpaceType referenceSpaces[referenceSpacesCount];
	result = xrEnumerateReferenceSpaces(session, referenceSpacesCount,
	                                    &referenceSpacesCount, referenceSpaces);
	if (!xr_result(instance, result, "Enumerating reference spaces failed!"))
		return 1;

	for (uint32_t i = 0; i < referenceSpacesCount; i++) {
		if (referenceSpaces[i] == type)
			return true;
	}
	return false;
}

void
deinit_openxr(OPENXR_API_HANDLE _self)
{
	xr_api* self = (xr_api*)_self;
	if (self->session) {
		xrDestroySession(self->session);
	}
	xrDestroyInstance(self->instance);
}

OPENXR_API_HANDLE
init_openxr()
{
	xr_api* self = malloc(sizeof(xr_api));
	XrResult result;

	uint32_t extensionCount = 0;
	result =
	    xrEnumerateInstanceExtensionProperties(NULL, 0, &extensionCount, NULL);

	/* TODO: instance null will not be able to convert XrResult to string */
	if (!xr_result(NULL, result,
	               "Failed to enumerate number of extension properties"))
		return NULL;

	XrExtensionProperties extensionProperties[extensionCount];
	for (uint16_t i = 0; i < extensionCount; i++) {
		extensionProperties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
		extensionProperties[i].next = NULL;
	}

	result = xrEnumerateInstanceExtensionProperties(
	    NULL, extensionCount, &extensionCount, extensionProperties);
	if (!xr_result(NULL, result, "Failed to enumerate extension properties"))
		return NULL;

	if (!isExtensionSupported(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
	                          extensionProperties, extensionCount)) {
		printf("Runtime does not support OpenGL extension!\n");
		return NULL;
	}

	const char* const enabledExtensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo instanceCreateInfo = {
	    .type = XR_TYPE_INSTANCE_CREATE_INFO,
	    .next = NULL,
	    .createFlags = 0,
	    .enabledExtensionCount =
	        sizeof(enabledExtensions) / sizeof(enabledExtensions[0]),
	    .enabledExtensionNames = enabledExtensions,
	    .enabledApiLayerCount = 0,
	    .applicationInfo =
	        {
	            // TODO: get application name from godot
	            // TODO: establish godot version -> uint32_t versioning
	            .applicationName = "Godot OpenXR Plugin",
	            .engineName = "Godot Engine",
	            .applicationVersion = 1,
	            .engineVersion = 0,
	            .apiVersion = XR_CURRENT_API_VERSION,
	        },
	};
	result = xrCreateInstance(&instanceCreateInfo, &self->instance);
	if (!xr_result(NULL, result, "Failed to create XR instance."))
		return NULL;

	XrSystemGetInfo systemGetInfo = {.type = XR_TYPE_SYSTEM_GET_INFO,
	                                 .formFactor =
	                                     XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
	                                 .next = NULL};

	XrSystemId systemId;
	result = xrGetSystem(self->instance, &systemGetInfo, &systemId);
	if (!xr_result(self->instance, result,
	               "Failed to get system for HMD form factor."))
		return NULL;

	XrSystemProperties systemProperties = {
	    .type = XR_TYPE_SYSTEM_PROPERTIES,
	    .next = NULL,
	    .graphicsProperties = {0},
	    .trackingProperties = {0},
	};
	result = xrGetSystemProperties(self->instance, systemId, &systemProperties);
	if (!xr_result(self->instance, result, "Failed to get System properties"))
		return NULL;


	XrViewConfigurationType viewConfigType =
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!isViewConfigSupported(self->instance, viewConfigType, systemId)) {
		printf("Stereo View Configuration not supported!");
		return NULL;
	}

	result = xrEnumerateViewConfigurationViews(
	    self->instance, systemId, viewConfigType, 0, &self->view_count, NULL);
	if (!xr_result(self->instance, result,
	               "Failed to get view configuration view count!"))
		return NULL;

	self->configuration_views =
	    malloc(sizeof(XrViewConfigurationView) * self->view_count);

	result = xrEnumerateViewConfigurationViews(
	    self->instance, systemId, viewConfigType, self->view_count,
	    &self->view_count, self->configuration_views);
	if (!xr_result(self->instance, result,
	               "Failed to enumerate view configuration views!"))
		return NULL;


	{
		XrGraphicsRequirementsOpenGLKHR opengl_reqs = {
		    .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR, .next = NULL};
		result = xrGetOpenGLGraphicsRequirementsKHR(self->instance, systemId,
		                                            &opengl_reqs);
		if (!xr_result(self->instance, result,
		               "Failed to get OpenGL graphics requirements!"))
			return NULL;

		XrVersion desired_opengl_version = XR_MAKE_VERSION(4, 5, 0);
		if (desired_opengl_version > opengl_reqs.maxApiVersionSupported ||
		    desired_opengl_version < opengl_reqs.minApiVersionSupported) {
			printf("Runtime does not support OpenGL Version 4.5.0!\n");
			return NULL;
		}
	}

	self->graphics_binding_gl = (XrGraphicsBindingOpenGLXlibKHR){
	    .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR,
	};

	/*
	  if (!initGL(&self->graphics_binding_gl.xDisplay,
	              &self->graphics_binding_gl.visualid,
	              &self->graphics_binding_gl.glxFBConfig,
	              &self->graphics_binding_gl.glxDrawable,
	              &self->graphics_binding_gl.glxContext,
	              self->configuration_views[0].recommendedImageRectWidth,
	              self->configuration_views[0].recommendedImageRectHeight)) {
	    printf("GL init failed!\n");
	    return 1;
	  }
	  */
	self->graphics_binding_gl.xDisplay = XOpenDisplay(NULL);
	self->graphics_binding_gl.glxContext = glXGetCurrentContext();
	self->graphics_binding_gl.glxDrawable = glXGetCurrentDrawable();

	printf("Graphics: Display %p, Context %" PRIxPTR ", Drawable %" PRIxPTR "\n",
	       self->graphics_binding_gl.xDisplay,
	       (uintptr_t) self->graphics_binding_gl.glxContext,
	       (uintptr_t) self->graphics_binding_gl.glxDrawable);

	printf("Using OpenGL version: %s\n", glGetString(GL_VERSION));
	printf("Using OpenGL Renderer: %s\n", glGetString(GL_RENDERER));

	XrSessionCreateInfo session_create_info = {.type =
	                                               XR_TYPE_SESSION_CREATE_INFO,
	                                           .next = &self->graphics_binding_gl,
	                                           .systemId = systemId};


	result =
	    xrCreateSession(self->instance, &session_create_info, &self->session);
	if (!xr_result(self->instance, result, "Failed to create session"))
		return NULL;


	printf("Created session\n");


	XrReferenceSpaceType playSpace = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!isReferenceSpaceSupported(self->instance, self->session, playSpace)) {
		printf("runtime does not support local space!\n");
		return NULL;
	}

	XrPosef identityPose = {.orientation = {.x = 0, .y = 0, .z = 0, .w = 1.0},
	                        .position = {.x = 0, .y = 0, .z = 0}};

	XrReferenceSpaceCreateInfo localSpaceCreateInfo = {
	    .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	    .next = NULL,
	    .referenceSpaceType = playSpace,
	    .poseInReferenceSpace = identityPose};

	result = xrCreateReferenceSpace(self->session, &localSpaceCreateInfo,
	                                &self->local_space);
	if (!xr_result(self->instance, result, "Failed to create local space!"))
		return NULL;

	XrSessionBeginInfo sessionBeginInfo = {.type = XR_TYPE_SESSION_BEGIN_INFO,
	                                       .next = NULL,
	                                       .primaryViewConfigurationType =
	                                           viewConfigType};
	result = xrBeginSession(self->session, &sessionBeginInfo);
	if (!xr_result(self->instance, result, "Failed to begin session!"))
		return NULL;

	uint32_t swapchainFormatCount;
	result = xrEnumerateSwapchainFormats(self->session, 0, &swapchainFormatCount,
	                                     NULL);
	if (!xr_result(self->instance, result,
	               "Failed to get number of supported swapchain formats"))
		return NULL;

	int64_t swapchainFormats[swapchainFormatCount];
	result = xrEnumerateSwapchainFormats(self->session, swapchainFormatCount,
	                                     &swapchainFormatCount, swapchainFormats);
	if (!xr_result(self->instance, result,
	               "Failed to enumerate swapchain formats"))
		return NULL;

	int64_t swapchainFormatToUse = swapchainFormats[0];
	self->swapchains = malloc(sizeof(XrSwapchain) * self->view_count);
	uint32_t swapchainLength[self->view_count];
	for (uint32_t i = 0; i < self->view_count; i++) {
		XrSwapchainCreateInfo swapchainCreateInfo = {
		    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		    .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
		                  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
		    .createFlags = 0,
		    .format = swapchainFormatToUse,
		    .sampleCount = 1,
		    .width = self->configuration_views[i].recommendedImageRectWidth,
		    .height = self->configuration_views[i].recommendedImageRectHeight,
		    .faceCount = 1,
		    .arraySize = 1,
		    .mipCount = 1,
		    .next = NULL,
		};
		result = xrCreateSwapchain(self->session, &swapchainCreateInfo,
		                           &self->swapchains[i]);
		if (!xr_result(self->instance, result, "Failed to create swapchain %d!", i))
			return NULL;
		result = xrEnumerateSwapchainImages(self->swapchains[i], 0,
		                                    &swapchainLength[i], NULL);
		if (!xr_result(self->instance, result, "Failed to enumerate swapchains"))
			return NULL;
	}

	uint32_t maxSwapchainLength = 0;
	for (uint32_t i = 0; i < self->view_count; i++) {
		if (swapchainLength[i] > maxSwapchainLength) {
			maxSwapchainLength = swapchainLength[i];
		}
	}

	self->images = malloc(sizeof(XrSwapchainImageOpenGLKHR*) * self->view_count);
	for (uint32_t i = 0; i < self->view_count; i++) {
		self->images[i] =
		    malloc(sizeof(XrSwapchainImageOpenGLKHR) * maxSwapchainLength);

		for (int j = 0; j < maxSwapchainLength; j++) {
			self->images[i][j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
			self->images[i][j].next = NULL;
		}
    }

	// self->framebuffers = malloc(sizeof(GLuint*) * self->view_count);
	// for (uint32_t i = 0; i < self->view_count; i++)
	//	self->framebuffers[i] = malloc(sizeof(GLuint) * maxSwapchainLength);


	for (uint32_t i = 0; i < self->view_count; i++) {
		result = xrEnumerateSwapchainImages(
		    self->swapchains[i], swapchainLength[i], &swapchainLength[i],
		    (XrSwapchainImageBaseHeader*)self->images[i]);
		if (!xr_result(self->instance, result,
		               "Failed to enumerate swapchain images"))
			return NULL;

		//	glGenFramebuffers(swapchainLength[i], self->framebuffers[i]);
	}


	// only used for OpenGL depth testing
	/*
	glGenTextures(1, &self->depthbuffer);
	glBindTexture(GL_TEXTURE_2D, self->depthbuffer);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
	             self->configuration_views[0].recommendedImageRectWidth,
	             self->configuration_views[0].recommendedImageRectHeight, 0,
	             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
*/

	self->projectionLayer = malloc(sizeof(XrCompositionLayerProjection));
	self->projectionLayer->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	self->projectionLayer->next = NULL;
	self->projectionLayer->layerFlags = 0;
	self->projectionLayer->space = self->local_space;
	self->projectionLayer->viewCount = self->view_count;
	self->projectionLayer->views = NULL;

	self->frameState = malloc(sizeof(XrFrameState));
	self->frameState->type = XR_TYPE_FRAME_STATE;
	self->frameState->next = NULL;

	// we will be made visiblke by runtime events
	self->visible = false;
	self->running = true;

	self->views = malloc(sizeof(XrView) * self->view_count);
	self->projection_views =
	    malloc(sizeof(XrCompositionLayerProjectionView) * self->view_count);
	for (uint32_t i = 0; i < self->view_count; i++) {
		self->views[i].type = XR_TYPE_VIEW;
		self->views[i].next = NULL;
		self->projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		self->projection_views[i].next = NULL;
	};

	return (OPENXR_API_HANDLE)self;
}

void
render_openxr(OPENXR_API_HANDLE _self, int eye, uint32_t texid)
{
	xr_api* self = (xr_api*)_self;
	// printf("Render eye %d texture %d\n", eye, texid);
	XrResult result;

	// if eye == 0, begin frame and do set up, render left eye.
	// if eye == 1, render right eye and endframe.
	// TODO: HMDs with more than 2 views.

	if (eye == 0) {
		XrFrameWaitInfo frameWaitInfo = {.type = XR_TYPE_FRAME_WAIT_INFO,
		                                 .next = NULL};
		result = xrWaitFrame(self->session, &frameWaitInfo, self->frameState);
		if (!xr_result(self->instance, result,
		               "xrWaitFrame() was not successful, exiting..."))
			return;

		XrEventDataBuffer runtimeEvent = {.type = XR_TYPE_EVENT_DATA_BUFFER,
		                                  .next = NULL};
		XrResult pollResult = xrPollEvent(self->instance, &runtimeEvent);
		if (pollResult == XR_SUCCESS) {
			switch (runtimeEvent.type) {
			case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
				printf("EVENT: events data lost!\n");
				XrEventDataEventsLost* event = (XrEventDataEventsLost*)&runtimeEvent;
				// do we care if the runtmime loses events?
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				printf("EVENT: instance loss pending!\n");
				XrEventDataInstanceLossPending* event =
				    (XrEventDataInstanceLossPending*)&runtimeEvent;
				self->running = false;
				return;
			}
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				printf("EVENT: session state changed ");
				XrEventDataSessionStateChanged* event =
				    (XrEventDataSessionStateChanged*)&runtimeEvent;
				XrSessionState state = event->state;

				// it would be better to handle each state change
				self->visible = event->state <= XR_SESSION_STATE_FOCUSED;
				printf("to %d. Visible: %d", state, self->visible);
				if (event->state >= XR_SESSION_STATE_STOPPING) {
					printf("Abort Mission!");
					self->running = false;
				}
				printf("\n");
				return;
			}
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				printf("EVENT: reference space change pengind!\n");
				XrEventDataReferenceSpaceChangePending* event =
				    (XrEventDataReferenceSpaceChangePending*)&runtimeEvent;
				// TODO: do something
				break;
			}
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
				printf("EVENT: interaction profile changed!\n");
				XrEventDataInteractionProfileChanged* event =
				    (XrEventDataInteractionProfileChanged*)&runtimeEvent;
				// TODO: do something
				break;
			}
			default: printf("Unhandled event type %d\n", runtimeEvent.type); break;
			}
		} else if (pollResult == XR_EVENT_UNAVAILABLE) {
			// this is the usual case
		} else {
			printf("Failed to poll events!\n");
			return;
		}

		if (!self->running || !self->visible)
			return;

		XrViewLocateInfo viewLocateInfo = {
		    .type = XR_TYPE_VIEW_LOCATE_INFO,
		    .displayTime = self->frameState->predictedDisplayTime,
		    .space = self->local_space};
		XrViewState viewState = {.type = XR_TYPE_VIEW_STATE, .next = NULL};
		int32_t viewCountOutput;
		result = xrLocateViews(self->session, &viewLocateInfo, &viewState,
		                       self->view_count, &viewCountOutput, self->views);
		if (!xr_result(self->instance, result, "Could not locate views"))
			return;

		XrFrameBeginInfo frameBeginInfo = {.type = XR_TYPE_FRAME_BEGIN_INFO,
		                                   .next = NULL};

		result = xrBeginFrame(self->session, &frameBeginInfo);
		if (!xr_result(self->instance, result, "failed to begin frame!"))
			return;
	}

	// common render code
	if (eye == 0 || eye == 1) {
		XrSwapchainImageAcquireInfo swapchainImageAcquireInfo = {
		    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = NULL};
		uint32_t bufferIndex;
		result = xrAcquireSwapchainImage(self->swapchains[eye],
		                                 &swapchainImageAcquireInfo, &bufferIndex);
		if (!xr_result(self->instance, result,
		               "failed to acquire swapchain image!"))
			return;

		XrSwapchainImageWaitInfo swapchainImageWaitInfo = {
		    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
		    .next = NULL,
		    .timeout = 1000};
		result =
		    xrWaitSwapchainImage(self->swapchains[eye], &swapchainImageWaitInfo);
		if (!xr_result(self->instance, result,
		               "failed to wait for swapchain image!"))
			return;


		self->projection_views[eye].type =
		    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		self->projection_views[eye].next = NULL;
		self->projection_views[eye].pose = self->views[eye].pose;
		self->projection_views[eye].fov = self->views[eye].fov;

		self->projection_views[eye].subImage.swapchain = self->swapchains[eye];
		self->projection_views[eye].subImage.imageArrayIndex = bufferIndex;
		self->projection_views[eye].subImage.imageRect.offset.x = 0;
		self->projection_views[eye].subImage.imageRect.offset.y = 0;
		self->projection_views[eye].subImage.imageRect.extent.width =
		    self->configuration_views[eye].recommendedImageRectWidth;
		self->projection_views[eye].subImage.imageRect.extent.height =
		    self->configuration_views[eye].recommendedImageRectHeight;

		XrSwapchainImageReleaseInfo swapchainImageReleaseInfo = {
		    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = NULL};
		result = xrReleaseSwapchainImage(self->swapchains[eye],
		                                 &swapchainImageReleaseInfo);
		if (!xr_result(self->instance, result,
		               "failed to release swapchain image!"))
			return;

		// we can't tell godot to render into our texture, so we just copy godot's
		// texture to our texture
		// besides, godot always uses the same texture
		glBindTexture(GL_TEXTURE_2D, texid);
		glCopyTextureSubImage2D(
		    self->images[eye][bufferIndex].image, 0, 0, 0, 0, 0,
		    self->configuration_views[eye].recommendedImageRectWidth,
		    self->configuration_views[eye].recommendedImageRectHeight);
		glBindTexture(GL_TEXTURE_2D, 0);
		// printf("Copy godot texture %d into XR texture %d\n", texid,
		// self->images[eye][bufferIndex].image);
	}

	if (eye == 1) {
		self->projectionLayer->views = self->projection_views;

		const XrCompositionLayerBaseHeader* const projectionlayers[1] = {
		    (const XrCompositionLayerBaseHeader* const)self->projectionLayer};
		XrFrameEndInfo frameEndInfo = {
		    .type = XR_TYPE_FRAME_END_INFO,
		    .displayTime = self->frameState->predictedDisplayTime,
		    .layerCount = 1,
		    .layers = projectionlayers,
		    .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		    .next = NULL};
		result = xrEndFrame(self->session, &frameEndInfo);
		if (!xr_result(self->instance, result, "failed to end frame!"))
			return;
	}
}

void
fill_projection_matrix(OPENXR_API_HANDLE _self, int eye, XrMatrix4x4f* matrix)
{
	xr_api* self = (xr_api*)_self;
	XrView views[self->view_count];
	for (uint32_t i = 0; i < self->view_count; i++) {
		views[i].type = XR_TYPE_VIEW;
		views[i].next = NULL;
	};

	XrViewLocateInfo viewLocateInfo = {
	    .type = XR_TYPE_VIEW_LOCATE_INFO,
	    .displayTime = 0, // TODO!!! frameState.predictedDisplayTime,
	    .space = self->local_space};

	XrViewState viewState = {.type = XR_TYPE_VIEW_STATE, .next = NULL};
	uint32_t viewCountOutput;
	XrResult result = xrLocateViews(self->session, &viewLocateInfo, &viewState,
	                                self->view_count, &viewCountOutput, views);

	// printf("FOV %f %f %f %f\n", views[eye].fov.angleLeft,
	// views[eye].fov.angleRight, views[eye].fov.angleUp,
	// views[eye].fov.angleDown);

	if (!xr_result(self->instance, result, "Could not locate views")) {
		printf("Locate Views failed??\n");
	} else {
		XrMatrix4x4f_CreateProjectionFov(matrix, GRAPHICS_OPENGL, views[eye].fov,
		                                 0.05f, 100.0f);
		// printf("Fill projection matrix for eye %d / %d\n", eye, self->view_count
		// - 1);
	}
}

void
recommended_rendertarget_size(OPENXR_API_HANDLE _self,
                              uint32_t* width,
                              uint32_t* height)
{
	xr_api* self = (xr_api*)_self;
	*width = self->configuration_views[0].recommendedImageRectWidth;
	*height = self->configuration_views[0].recommendedImageRectHeight;
}

bool
get_view_matrix(OPENXR_API_HANDLE _self, int eye, XrMatrix4x4f* matrix)
{
	xr_api* self = (xr_api*)_self;
	if (self->views == NULL)
		return false;
	const XrVector3f uniformScale = {.x = 1.f, .y = 1.f, .z = 1.f};

	XrMatrix4x4f viewMatrix;
	XrMatrix4x4f_CreateTranslationRotationScaleOrbit(
	    &viewMatrix, &self->views[eye].pose.position,
	    &self->views[eye].pose.orientation, &uniformScale);

	// Calculates the inverse of a rigid body transform.
	XrMatrix4x4f inverseViewMatrix;
	XrMatrix4x4f_InvertRigidBody(matrix, &viewMatrix);
	return true;
}