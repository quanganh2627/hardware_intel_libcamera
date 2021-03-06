/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "Camera_Driver"

#include <Exif.h>
#include "LogHelper.h"
#include "CameraDriver.h"
#include "Callbacks.h"
#include "ColorConverter.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <cutils/properties.h>

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define DEFAULT_SENSOR_FPS      15.0

#define RESOLUTION_14MP_WIDTH   4352
#define RESOLUTION_14MP_HEIGHT  3264
#define RESOLUTION_8MP_WIDTH    3264
#define RESOLUTION_8MP_HEIGHT   2448
#define RESOLUTION_5MP_WIDTH    2560
#define RESOLUTION_5MP_HEIGHT   1920
#define RESOLUTION_1080P_WIDTH  1920
#define RESOLUTION_1080P_HEIGHT 1080
#define RESOLUTION_720P_WIDTH   1280
#define RESOLUTION_720P_HEIGHT  720
#define RESOLUTION_480P_WIDTH   768
#define RESOLUTION_480P_HEIGHT  480
#define RESOLUTION_VGA_WIDTH    640
#define RESOLUTION_VGA_HEIGHT   480
#define RESOLUTION_POSTVIEW_WIDTH    320
#define RESOLUTION_POSTVIEW_HEIGHT   240

// TODO: revisit this legacy implementation
#define MAX_BACK_CAMERA_PREVIEW_WIDTH           RESOLUTION_1080P_WIDTH
#define MAX_BACK_CAMERA_PREVIEW_HEIGHT          RESOLUTION_1080P_HEIGHT
#define MAX_BACK_CAMERA_SNAPSHOT_WIDTH          RESOLUTION_1080P_WIDTH
#define MAX_BACK_CAMERA_SNAPSHOT_HEIGHT         RESOLUTION_1080P_HEIGHT
#define MAX_BACK_CAMERA_VIDEO_WIDTH             RESOLUTION_1080P_WIDTH
#define MAX_BACK_CAMERA_VIDEO_HEIGHT            RESOLUTION_1080P_HEIGHT

#define MAX_FRONT_CAMERA_PREVIEW_WIDTH          RESOLUTION_1080P_WIDTH
#define MAX_FRONT_CAMERA_PREVIEW_HEIGHT         RESOLUTION_1080P_HEIGHT
#define MAX_FRONT_CAMERA_SNAPSHOT_WIDTH         RESOLUTION_1080P_WIDTH
#define MAX_FRONT_CAMERA_SNAPSHOT_HEIGHT        RESOLUTION_1080P_HEIGHT
#define MAX_FRONT_CAMERA_VIDEO_WIDTH            RESOLUTION_1080P_WIDTH
#define MAX_FRONT_CAMERA_VIDEO_HEIGHT           RESOLUTION_1080P_HEIGHT

namespace android {

////////////////////////////////////////////////////////////////////
//                          STATIC DATA
////////////////////////////////////////////////////////////////////

CameraDriver::CameraSensor *CameraDriver::mCameraSensor[MAX_CAMERAS];
Mutex CameraDriver::mCameraSensorLock;
int CameraDriver::numCameras = 0;

////////////////////////////////////////////////////////////////////
//                          PUBLIC METHODS
////////////////////////////////////////////////////////////////////

CameraDriver::CameraDriver(int cameraId) :
    mMode(MODE_NONE)
    ,mCallbacks(Callbacks::getInstance())
    ,mSessionId(0)
    ,mCameraId(cameraId)
    ,mFormat(V4L2_PIX_FMT_YUYV)
{
    LOG1("@%s", __FUNCTION__);

    mConfig.fps = 30;
    mConfig.num_snapshot = 1;
    mConfig.zoom = 0;

    memset(&mBufferPool, 0, sizeof(mBufferPool));

    int ret = openDevice();
    if (ret < 0) {
        ALOGE("Failed to open device!");
        return;
    }

    ret = detectDeviceResolutions();
    if (ret) {
        ALOGE("Failed to detect camera resolution! Use default settings");
        if (mCameraSensor[cameraId]->info.facing == CAMERA_FACING_FRONT) {
            mConfig.snapshot.maxWidth  = MAX_FRONT_CAMERA_SNAPSHOT_WIDTH;
            mConfig.snapshot.maxHeight = MAX_FRONT_CAMERA_SNAPSHOT_HEIGHT;
        } else {
            mConfig.snapshot.maxWidth  = MAX_BACK_CAMERA_SNAPSHOT_WIDTH;
            mConfig.snapshot.maxHeight = MAX_BACK_CAMERA_SNAPSHOT_HEIGHT;
        }
    } else {
        LOG1("Max-resolution detected: %dx%d",
                mConfig.snapshot.maxWidth,
                mConfig.snapshot.maxHeight);
    }

    if (mCameraSensor[cameraId]->info.facing == CAMERA_FACING_FRONT) {
        mConfig.preview.maxWidth   = MAX_FRONT_CAMERA_PREVIEW_WIDTH;
        mConfig.preview.maxHeight  = MAX_FRONT_CAMERA_PREVIEW_HEIGHT;
        mConfig.recording.maxWidth = MAX_FRONT_CAMERA_VIDEO_WIDTH;
        mConfig.recording.maxHeight = MAX_FRONT_CAMERA_VIDEO_HEIGHT;
    } else {
        mConfig.preview.maxWidth   = MAX_BACK_CAMERA_PREVIEW_WIDTH;
        mConfig.preview.maxHeight  = MAX_BACK_CAMERA_PREVIEW_HEIGHT;
        mConfig.recording.maxWidth = MAX_BACK_CAMERA_VIDEO_WIDTH;
        mConfig.recording.maxHeight = MAX_BACK_CAMERA_VIDEO_HEIGHT;
    }

    // Initialize the frame sizes (TODO: decide on appropriate sizes)
    setPreviewFrameSize(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT);
    setPostviewFrameSize(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT);
    setSnapshotFrameSize(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT);
    setVideoFrameSize(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT);

    closeDevice();
}

CameraDriver::~CameraDriver()
{
    LOG1("@%s", __FUNCTION__);
    /*
     * The destructor is called when the hw_module close mehod is called. The close method is called
     * in general by the camera client when it's done with the camera device, but it is also called by
     * System Server when the camera application crashes. System Server calls close in order to release
     * the camera hardware module. So, if we are not in MODE_NONE, it means that we are in the middle of
     * somthing when the close function was called. So it's our duty to stop first, then close the
     * camera device.
     */
    if (mMode != MODE_NONE) {
        stop();
    }
}

void CameraDriver::getDefaultParameters(CameraParameters *params)
{
    LOG2("@%s", __FUNCTION__);
    if (!params) {
        ALOGE("params is null!");
        return;
    }

    /**
     * PREVIEW
     */
    params->setPreviewSize(mConfig.preview.width, mConfig.preview.height);
    params->setPreviewFrameRate(30);

    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, "640x480");

    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,"30"); // TODO: consider which FPS to support
    params->set(CameraParameters::KEY_PREVIEW_FPS_RANGE,"30000,30000");
    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,"(30000,30000)");

    /**
     * RECORDING
     */
    params->setVideoSize(mConfig.recording.width, mConfig.recording.height);
    params->set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "640x480");
    params->set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES, ""); // empty string indicates we only support a single stream

    params->set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED, CameraParameters::FALSE);

    /**
     * SNAPSHOT
     */
    params->set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, "640x480");
    params->setPictureSize(mConfig.snapshot.width, mConfig.snapshot.height);
    params->set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES,"0x0"); // 0x0 indicates "not supported"
    params->set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, 0);
    params->set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, 0);

    /**
     * ZOOM
     */
    params->set(CameraParameters::KEY_ZOOM, 0);
    params->set(CameraParameters::KEY_ZOOM_SUPPORTED, CameraParameters::TRUE);
    getZoomRatios(MODE_PREVIEW, params);

    /**
     * FLASH
     */
    params->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
    params->set(CameraParameters::KEY_SUPPORTED_FLASH_MODES, CameraParameters::FLASH_MODE_OFF);

    /**
     * FOCUS
     */
    params->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_FIXED);
    params->set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES, CameraParameters::FOCUS_MODE_FIXED);

    /**
     * FOCAL LENGTH
     */
    // TODO: find out actual focal length
    // TODO: also find out how to get sensor width and height which will likely be used with focal length
    float focalLength = 0.0; // focalLength unit is mm
    params->setFloat(CameraParameters::KEY_FOCAL_LENGTH, focalLength);

    /**
     * FOCUS DISTANCES
     */
    getFocusDistances(params);

    /**
     * MISCELLANEOUS
     */
    params->set(CameraParameters::KEY_VERTICAL_VIEW_ANGLE,"0.0");
    params->set(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE,"0.0");

    /**
     * EXPOSURE
     */
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION,0);
    params->set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION,0);
    params->set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION,0);
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP,0);

    // effect modes
    params->set(CameraParameters::KEY_EFFECT, CameraParameters::EFFECT_NONE);
    params->set(CameraParameters::KEY_SUPPORTED_EFFECTS, CameraParameters::EFFECT_NONE);

    // white-balance mode
    params->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
    params->set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);

    // scene mode
    params->set(CameraParameters::KEY_SCENE_MODE, CameraParameters::SCENE_MODE_AUTO);
    params->set(CameraParameters::KEY_SUPPORTED_SCENE_MODES, CameraParameters::SCENE_MODE_AUTO);

    // 3a lock: auto-exposure lock
    params->set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK, "");
    params->set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK_SUPPORTED, CameraParameters::FALSE);

    // 3a lock: auto-whitebalance lock
    params->set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK, "");
    params->set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED, CameraParameters::FALSE);

    // multipoint focus
    params->set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS, 0);

    // set empty area
    params->set(CameraParameters::KEY_FOCUS_AREAS, "");

    // metering areas
    params->set(CameraParameters::KEY_MAX_NUM_METERING_AREAS, 0);
}

status_t CameraDriver::start(Mode mode)
{
    LOG1("@%s", __FUNCTION__);
    LOG1("mode = %d", mode);
    status_t status = NO_ERROR;

    switch (mode) {
    case MODE_PREVIEW:
        status = startPreview();
        break;

    case MODE_VIDEO:
        status = startRecording();
        break;

    case MODE_CAPTURE:
        status = startCapture();
        break;

    default:
        break;
    };

    if (status == NO_ERROR) {
        mMode = mode;
        mSessionId++;
    }

    return status;
}

status_t CameraDriver::stop()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    switch (mMode) {
    case MODE_PREVIEW:
        status = stopPreview();
        break;

    case MODE_VIDEO:
        status = stopRecording();
        break;

    case MODE_CAPTURE:
        status = stopCapture();
        break;

    default:
        break;
    };

    if (status == NO_ERROR)
        mMode = MODE_NONE;

    return status;
}

status_t CameraDriver::startPreview()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;

    ret = openDevice();
    if (ret < 0) {
        ALOGE("Open device failed!");
        status = UNKNOWN_ERROR;
        return status;
    }

    ret = configureDevice(
            MODE_PREVIEW,
            mConfig.preview.padding,
            mConfig.preview.height,
            NUM_DEFAULT_BUFFERS);
    if (ret < 0) {
        ALOGE("Configure device failed!");
        status = UNKNOWN_ERROR;
        goto exitClose;
    }

    // need to resend the current zoom value
    set_zoom(mCameraSensor[mCameraId]->fd, mConfig.zoom);

    ret = startDevice();
    if (ret < 0) {
        ALOGE("Start device failed!");
        status = UNKNOWN_ERROR;
        goto exitDeconfig;
    }

    return status;

exitDeconfig:
    deconfigureDevice();
exitClose:
    closeDevice();
    return status;
}

status_t CameraDriver::stopPreview()
{
    LOG1("@%s", __FUNCTION__);

    stopDevice();
    deconfigureDevice();
    closeDevice();

    return NO_ERROR;
}

status_t CameraDriver::startRecording()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;

    ret = openDevice();
    if (ret < 0) {
        ALOGE("Open device failed!");
        status = UNKNOWN_ERROR;
        return status;
    }

    ret = configureDevice(
            MODE_VIDEO,
            mConfig.preview.padding,
            mConfig.preview.height,
            NUM_DEFAULT_BUFFERS);
    if (ret < 0) {
        ALOGE("Configure device failed!");
        status = UNKNOWN_ERROR;
        goto exitClose;
    }

    ret = startDevice();
    if (ret < 0) {
        ALOGE("Start device failed!");
        status = UNKNOWN_ERROR;
        goto exitDeconfig;
    }

    return status;

exitDeconfig:
    deconfigureDevice();
exitClose:
    closeDevice();
    return status;
}

status_t CameraDriver::stopRecording()
{
    LOG1("@%s", __FUNCTION__);

    stopDevice();
    deconfigureDevice();
    closeDevice();

    return NO_ERROR;
}

status_t CameraDriver::startCapture()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;

    ret = openDevice();
    if (ret < 0) {
        ALOGE("Open device failed!");
        status = UNKNOWN_ERROR;
        return status;
    }

    ret = configureDevice(
            MODE_CAPTURE,
            mConfig.preview.padding,
            mConfig.preview.height,
            NUM_DEFAULT_BUFFERS);
    if (ret < 0) {
        ALOGE("Configure device failed!");
        status = UNKNOWN_ERROR;
        goto exitClose;
    }

    // need to resend the current zoom value
    set_zoom(mCameraSensor[mCameraId]->fd, mConfig.zoom);

    ret = startDevice();
    if (ret < 0) {
        ALOGE("Start device failed!");
        status = UNKNOWN_ERROR;
        goto exitDeconfig;
    }

    return status;

exitDeconfig:
    deconfigureDevice();
exitClose:
    closeDevice();
    return status;
}

status_t CameraDriver::stopCapture()
{
    LOG1("@%s", __FUNCTION__);

    stopDevice();
    deconfigureDevice();
    closeDevice();

    return NO_ERROR;
}

int CameraDriver::configureDevice(Mode deviceMode, int w, int h, int numBuffers)
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    LOG1("width:%d, height:%d, deviceMode:%d",
            w, h, deviceMode);

    if ((w <= 0) || (h <= 0)) {
        ALOGE("Wrong Width %d or Height %d", w, h);
        return -1;
    }

    int fd = mCameraSensor[mCameraId]->fd;

    //Switch the Mode before set the format. This is a driver requirement
    ret = set_capture_mode(deviceMode);
    if (ret < 0)
        return ret;

    //Set the format
    ret = v4l2_capture_s_format(fd, w, h);
    if (ret < 0)
        return ret;

    ret = v4l2_capture_g_framerate(fd, &mConfig.fps, w, h);
    if (ret < 0) {
        /*Error handler: if driver does not support FPS achieving,
          just give the default value.*/
        mConfig.fps = DEFAULT_SENSOR_FPS;
        ret = 0;
    }

    status_t status = allocateBuffers(numBuffers);
    if (status != NO_ERROR) {
        ALOGE("error allocating buffers");
        ret = -1;
    }

    return ret;
}

int CameraDriver::deconfigureDevice()
{
    status_t status = freeBuffers();
    if (status != NO_ERROR) {
        ALOGE("Error freeing buffers");
        return -1;
    }
    return 0;
}

int CameraDriver::startDevice()
{
    LOG1("@%s fd=%d", __FUNCTION__, mCameraSensor[mCameraId]->fd);

    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0; i < mBufferPool.numBuffers; i++) {
        status_t status = queueBuffer(&mBufferPool.bufs[i].camBuff, true);
        if (status != NO_ERROR)
            return -1;
    }

    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        ALOGE("VIDIOC_STREAMON returned: %d (%s)", ret, strerror(errno));
        return ret;
    }

    return 0;
}

void CameraDriver::stopDevice()
{
    LOG1("@%s", __FUNCTION__);

    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        ALOGE("VIDIOC_STREAMOFF returned: %d (%s)", ret, strerror(errno));
    }
}

int CameraDriver::openDevice()
{
    int fd;
    LOG1("@%s", __FUNCTION__);
    if (mCameraSensor[mCameraId] == 0) {
        ALOGE("%s: Try to open non-existent camera", __FUNCTION__);
        return -ENODEV;
    }

    LOG1("@%s", __FUNCTION__);
    if (mCameraSensor[mCameraId]->fd >= 0) {
        ALOGE("%s: camera is already opened", __FUNCTION__);
        return mCameraSensor[mCameraId]->fd;
    }
    const char *dev_name = mCameraSensor[mCameraId]->devName;

    fd = v4l2_capture_open(dev_name);

    if (fd < 0) {
        ALOGE("V4L2: capture_open failed: %s", strerror(errno));
        return -EFAULT;
    }

    // Query and check the capabilities
    struct v4l2_capability cap;
    if (v4l2_capture_querycap(fd, &cap) < 0) {
        ALOGE("V4L2: capture_querycap failed: %s", strerror(errno));
        v4l2_capture_close(fd);
        return -EFAULT;
    }

    mCameraSensor[mCameraId]->fd = fd;

    return mCameraSensor[mCameraId]->fd;
}

void CameraDriver::closeDevice()
{
    LOG1("@%s", __FUNCTION__);

    if (mCameraSensor[mCameraId] == 0) {
        ALOGE("%s: Try to open non-existent camera", __FUNCTION__);
        return;
    }

    if (mCameraSensor[mCameraId]->fd < 0) {
        ALOGE("oh no. this should not be happening");
        return;
    }

    v4l2_capture_close(mCameraSensor[mCameraId]->fd);

    mCameraSensor[mCameraId]->fd = -1;
}
CameraBuffer* CameraDriver::findBuffer(void* findMe) const
{

    for (int i = 0; i < mBufferPool.numBuffers; i++) {
        if (mBufferPool.bufs[i].camBuff.getData() == findMe)
            return &(mBufferPool.bufs[i].camBuff);
    }
    return 0;
}

status_t CameraDriver::allocateBuffer(int fd, int index)
{
    struct v4l2_buffer *vbuf = &mBufferPool.bufs[index].vBuff;
    CameraBuffer *camBuf = &mBufferPool.bufs[index].camBuff;
    int ret;

    // query for buffer info
    vbuf->flags = 0x0;
    vbuf->index = index;
    vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf->memory = V4L2_MEMORY_USERPTR;
    ret = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
    if (ret < 0) {
        ALOGE("VIDIOC_QUERYBUF failed: %s", strerror(errno));
        return UNKNOWN_ERROR;
    }

    // allocate memory
    camBuf->mID = index;
    mCallbacks->allocateMemory(camBuf, vbuf->length);
    vbuf->m.userptr = (unsigned int) camBuf->getData();

    camBuf->setFormat(mFormat);

    LOG1("alloc mem addr=%p, index=%d size=%d", camBuf->getData(), index, vbuf->length);

    return NO_ERROR;
}

status_t CameraDriver::allocateBuffers(int numBuffers)
{
    if (mBufferPool.bufs) {
        ALOGE("fail to alloc. non-null buffs");
        return UNKNOWN_ERROR;
    }

    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    struct v4l2_requestbuffers reqBuf;
    reqBuf.count = numBuffers;
    reqBuf.memory = V4L2_MEMORY_USERPTR;
    reqBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    LOG1("VIDIOC_REQBUFS, count=%d", reqBuf.count);
    ret = ioctl(fd, VIDIOC_REQBUFS, &reqBuf);

    if (ret < 0) {
        ALOGE("VIDIOC_REQBUFS(%d) returned: %d (%s)",
            numBuffers, ret, strerror(errno));
        return UNKNOWN_ERROR;
    }

    mBufferPool.bufs = new DriverBuffer[numBuffers];

    status_t status = NO_ERROR;
    for (int i = 0; i < numBuffers; i++) {
        status = allocateBuffer(fd, i);
        if (status != NO_ERROR)
            goto fail;

        mBufferPool.numBuffers++;
    }

    return NO_ERROR;

fail:

    for (int i = 0; i < mBufferPool.numBuffers; i++) {
        freeBuffer(i);
    }

    delete [] mBufferPool.bufs;
    memset(&mBufferPool, 0, sizeof(mBufferPool));

    return status;
}

status_t CameraDriver::freeBuffer(int index)
{
    CameraBuffer *camBuf = &mBufferPool.bufs[index].camBuff;
    camBuf->releaseMemory();
    return NO_ERROR;
}

status_t CameraDriver::freeBuffers()
{
    if (!mBufferPool.bufs) {
        ALOGE("fail to free. null buffers");
        return NO_ERROR; // This is okay, just print an error
    }

    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    struct v4l2_requestbuffers reqBuf;
    reqBuf.count = 0;
    reqBuf.memory = V4L2_MEMORY_USERPTR;
    reqBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0; i < mBufferPool.numBuffers; i++) {
        freeBuffer(i);
    }

    LOG1("VIDIOC_REQBUFS, count=%d", reqBuf.count);
    ret = ioctl(fd, VIDIOC_REQBUFS, &reqBuf);

    if (ret < 0) {
        // Just print an error and continue with dealloc logic
        ALOGE("VIDIOC_REQBUFS returned: %d (%s)",
                ret, strerror(errno));
    }

    delete [] mBufferPool.bufs;
    memset(&mBufferPool, 0, sizeof(mBufferPool));

    return NO_ERROR;
}

status_t CameraDriver::queueBuffer(CameraBuffer *buff, bool init)
{
    // see if we are in session (not initializing the driver with buffers)
    if (init == false) {
        if (buff->mDriverPrivate != mSessionId)
            return DEAD_OBJECT;
    }

    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    struct v4l2_buffer *vbuff = &mBufferPool.bufs[buff->getID()].vBuff;

    ret = ioctl(fd, VIDIOC_QBUF, vbuff);
    if (ret < 0) {
        ALOGE("VIDIOC_QBUF index %d failed: %s",
             buff->getID(), strerror(errno));
        return UNKNOWN_ERROR;
    }

    mBufferPool.numBuffersQueued++;

    return NO_ERROR;
}

status_t CameraDriver::dequeueBuffer(CameraBuffer **buff, nsecs_t *timestamp)
{
    int ret;
    int fd = mCameraSensor[mCameraId]->fd;
    struct v4l2_buffer vbuff;

    vbuff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuff.memory = V4L2_MEMORY_USERPTR;

    ret = ioctl(fd, VIDIOC_DQBUF, &vbuff);
    if (ret < 0) {
        ALOGE("error dequeuing buffers");
        return UNKNOWN_ERROR;
    }

    CameraBuffer *camBuff = &mBufferPool.bufs[vbuff.index].camBuff;
    camBuff->mID = vbuff.index;
    camBuff->mDriverPrivate = mSessionId;
    *buff = camBuff;

    if (timestamp)
        *timestamp = systemTime();

    mBufferPool.numBuffersQueued--;

    return NO_ERROR;
}

int CameraDriver::detectDeviceResolutions()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    struct v4l2_frmsizeenum frame_size;

    //Switch the Mode before try the format.
    ret = set_capture_mode(MODE_CAPTURE);
    if (ret < 0)
        return ret;

    int i = 0;
    while (true) {
        memset(&frame_size, 0, sizeof(frame_size));
        frame_size.index = i++;
        frame_size.pixel_format = mFormat;
        /* TODO: Currently VIDIOC_ENUM_FRAMESIZES is returning with Invalid argument
         * Need to know why the driver is not supporting this V4L2 API call
         */
        if (ioctl(mCameraSensor[mCameraId]->fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) < 0) {
            break;
        }
        ret++;
        float fps = 0;
        v4l2_capture_g_framerate(
                mCameraSensor[mCameraId]->fd,
                &fps,
                frame_size.discrete.width,
                frame_size.discrete.height);
        LOG1("Supported frame size: %ux%u@%dfps",
                frame_size.discrete.width,
                frame_size.discrete.height,
                static_cast<int>(fps));
    }

    // Get the maximum format supported
    mConfig.snapshot.maxWidth = 0xffff;
    mConfig.snapshot.maxHeight = 0xffff;
    ret = v4l2_capture_try_format(mCameraSensor[mCameraId]->fd,
            &mConfig.snapshot.maxWidth,
            &mConfig.snapshot.maxHeight);
    if (ret < 0)
        return ret;
    return 0;
}

status_t CameraDriver::setPreviewFrameSize(int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (width > mConfig.preview.maxWidth || width <= 0)
        width = mConfig.preview.maxWidth;
    if (height > mConfig.preview.maxHeight || height <= 0)
        height = mConfig.preview.maxHeight;
    mConfig.preview.width = width;
    mConfig.preview.height = height;
    mConfig.preview.padding = paddingWidth(mFormat, width, height);
    mConfig.preview.size = frameSize(mFormat, mConfig.preview.padding, height);
    LOG1("width(%d), height(%d), pad_width(%d), size(%d)",
        width, height, mConfig.preview.padding, mConfig.preview.size);
    return status;
}

status_t CameraDriver::setPostviewFrameSize(int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    LOG1("width(%d), height(%d)",
         width, height);
    mConfig.postview.width = width;
    mConfig.postview.height = height;
    mConfig.postview.padding = paddingWidth(mFormat, width, height);
    mConfig.postview.size = frameSize(mFormat, width, height);
    if (mConfig.postview.size == 0)
        mConfig.postview.size = mConfig.postview.width * mConfig.postview.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d), size(%d)",
            width, height, mConfig.postview.padding, mConfig.postview.size);
    return status;
}

status_t CameraDriver::setSnapshotFrameSize(int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (width > mConfig.snapshot.maxWidth || width <= 0)
        width = mConfig.snapshot.maxWidth;
    if (height > mConfig.snapshot.maxHeight || height <= 0)
        height = mConfig.snapshot.maxHeight;
    mConfig.snapshot.width  = width;
    mConfig.snapshot.height = height;
    mConfig.snapshot.padding = paddingWidth(mFormat, width, height);
    mConfig.snapshot.size = frameSize(mFormat, width, height);;
    if (mConfig.snapshot.size == 0)
        mConfig.snapshot.size = mConfig.snapshot.width * mConfig.snapshot.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d), size(%d)",
        width, height, mConfig.snapshot.padding, mConfig.snapshot.size);
    return status;
}

void CameraDriver::getVideoSize(int *width, int *height)
{
    if (width && height) {
        *width = mConfig.recording.width;
        *height = mConfig.recording.height;
    }
}

status_t CameraDriver::setVideoFrameSize(int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;

    if (mConfig.recording.width == width &&
        mConfig.recording.height == height) {
        // Do nothing
        return status;
    }

    if (mMode == MODE_VIDEO) {
        ALOGE("Reconfiguration in video mode unsupported. Stop the driver first");
        return INVALID_OPERATION;
    }

    if (width > mConfig.recording.maxWidth || width <= 0) {
        ALOGE("invalid recording width %d. override to %d", width, mConfig.recording.maxWidth);
        width = mConfig.recording.maxWidth;
    }
    if (height > mConfig.recording.maxHeight || height <= 0) {
        ALOGE("invalid recording height %d. override to %d", height, mConfig.recording.maxHeight);
        height = mConfig.recording.maxHeight;
    }
    mConfig.recording.width = width;
    mConfig.recording.height = height;
    mConfig.recording.padding = paddingWidth(mFormat, width, height);
    mConfig.recording.size = frameSize(mFormat, width, height);
    if (mConfig.recording.size == 0)
        mConfig.recording.size = mConfig.recording.width * mConfig.recording.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d)",
            width, height, mConfig.recording.padding);

    return status;
}

void CameraDriver::getZoomRatios(Mode mode, CameraParameters *params)
{
    LOG1("@%s", __FUNCTION__);

    // TODO: decide if we can support zoom

    // zoom is not supported. this is indicated by placing a single zoom ratio in params
    params->set(CameraParameters::KEY_MAX_ZOOM, "0"); // zoom index 0 indicates first (and only) zoom ratio
    params->set(CameraParameters::KEY_ZOOM_RATIOS, "100");
}

void CameraDriver::getFocusDistances(CameraParameters *params)
{
    LOG1("@%s", __FUNCTION__);
    // TODO: set focus distances (CameraParameters::KEY_FOCUS_DISTANCES,)
}

status_t CameraDriver::setZoom(int zoom)
{
    LOG1("@%s: zoom = %d", __FUNCTION__, zoom);
    if (zoom == mConfig.zoom)
        return NO_ERROR;
    if (mMode == MODE_CAPTURE)
        return NO_ERROR;

    int ret = set_zoom(mCameraSensor[mCameraId]->fd, zoom);
    if (ret < 0) {
        ALOGE("Error setting zoom to %d", zoom);
        return UNKNOWN_ERROR;
    }
    mConfig.zoom = zoom;
    return NO_ERROR;
}

status_t CameraDriver::getFNumber(unsigned int *fNumber)
{
    LOG1("@%s", __FUNCTION__);

    // TODO: implement

    return NO_ERROR;
}

status_t CameraDriver::getExposureInfo(CamExifExposureProgramType *exposureProgram,
                                       CamExifExposureModeType *exposureMode,
                                       int *exposureTime,
                                       float *exposureBias,
                                       int *aperture)
{
    // TODO: fill these with valid values
    *exposureProgram = EXIF_EXPOSURE_PROGRAM_NORMAL;
    *exposureMode = EXIF_EXPOSURE_AUTO;
    *exposureTime = 0;
    *exposureBias = 0.0;
    *aperture = 1;
    return NO_ERROR;
}

status_t CameraDriver::getBrightness(float *brightness)
{
    // TODO: fill these with valid values
    *brightness = 0.0;
    return NO_ERROR;
}

status_t CameraDriver::getIsoSpeed(int *isoSpeed)
{
    // TODO: fill these with valid values
    *isoSpeed = 0;
    return NO_ERROR;
}

status_t CameraDriver::getMeteringMode(CamExifMeteringModeType *meteringMode)
{
    *meteringMode = EXIF_METERING_UNKNOWN;
    return NO_ERROR;
}

status_t CameraDriver::getAWBMode(CamExifWhiteBalanceType *wbMode)
{
    *wbMode = EXIF_WB_AUTO;
    return NO_ERROR;
}

status_t CameraDriver::getSceneMode(CamExifSceneCaptureType *sceneMode)
{
    *sceneMode = EXIF_SCENE_STANDARD;
    return NO_ERROR;
}

int CameraDriver::set_zoom(int fd, int zoom)
{
    LOG1("@%s", __FUNCTION__);

    // TODO: set zoom

    return NO_ERROR;
}

int CameraDriver::set_attribute (int fd, int attribute_num,
                                             const int value, const char *name)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_control control;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control ext_control;

    LOG1("setting attribute [%s] to %d", name, value);

    if (fd < 0)
        return -1;

    control.id = attribute_num;
    control.value = value;
    controls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
    controls.count = 1;
    controls.controls = &ext_control;
    ext_control.id = attribute_num;
    ext_control.value = value;

    if (ioctl(fd, VIDIOC_S_CTRL, &control) == 0)
        return 0;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0)
        return 0;

    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0)
        return 0;

    ALOGE("Failed to set value %d for control %s (%d) on fd '%d', %s",
        value, name, attribute_num, fd, strerror(errno));
    return -1;
}

int CameraDriver::xioctl(int fd, int request, void *arg)
{
    int ret;

    do {
        ret = ioctl (fd, request, arg);
    } while (-1 == ret && EINTR == errno);

    if (ret < 0)
        ALOGW ("Request %d failed: %s", request, strerror(errno));

    return ret;
}

int CameraDriver::v4l2_capture_g_framerate(int fd, float *framerate, int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_frmivalenum frm_interval;

    if (NULL == framerate)
        return -EINVAL;

    assert(fd > 0);
    CLEAR(frm_interval);
    frm_interval.pixel_format = mFormat;
    frm_interval.width = width;
    frm_interval.height = height;
    *framerate = -1.0;

    ret = ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frm_interval);
    if (ret < 0) {
        ALOGW("ioctl failed: %s", strerror(errno));
        return ret;
    }

    assert(0 != frm_interval.discrete.denominator);

    *framerate = 1.0 / (1.0 * frm_interval.discrete.numerator / frm_interval.discrete.denominator);

    return 0;
}

int CameraDriver::v4l2_capture_s_format(int fd, int w, int h)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_format v4l2_fmt;
    CLEAR(v4l2_fmt);

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    LOG1("VIDIOC_G_FMT");
    ret = ioctl (fd,  VIDIOC_G_FMT, &v4l2_fmt);
    if (ret < 0) {
        ALOGE("VIDIOC_G_FMT failed: %s", strerror(errno));
        return -1;
    }

    v4l2_fmt.fmt.pix.width = w;
    v4l2_fmt.fmt.pix.height = h;
    v4l2_fmt.fmt.pix.pixelformat = mFormat;
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
    LOG1("VIDIOC_S_FMT: width: %d, height: %d, format: %d, field: %d",
                v4l2_fmt.fmt.pix.width,
                v4l2_fmt.fmt.pix.height,
                v4l2_fmt.fmt.pix.pixelformat,
                v4l2_fmt.fmt.pix.field);
    ret = ioctl(fd, VIDIOC_S_FMT, &v4l2_fmt);
    if (ret < 0) {
        ALOGE("VIDIOC_S_FMT failed: %s", strerror(errno));
        return -1;
    }
    return 0;

}

status_t CameraDriver::v4l2_capture_open(const char *devName)
{
    LOG1("@%s", __FUNCTION__);
    int fd;
    struct stat st;

    LOG1("---Open video device %s---", devName);

    if (stat (devName, &st) == -1) {
        ALOGE("Error stat video device %s: %s",
                devName, strerror(errno));
        return -1;
    }

    if (!S_ISCHR (st.st_mode)) {
        ALOGE("%s is not a device", devName);
        return -1;
    }

    fd = open(devName, O_RDWR);

    if (fd <= 0) {
        ALOGE("Error opening video device %s: %s",
                devName, strerror(errno));
        return -1;
    }

    return fd;
}

status_t CameraDriver::v4l2_capture_close(int fd)
{
    LOG1("@%s", __FUNCTION__);
    /* close video device */
    LOG1("----close device ---");
    if (fd < 0) {
        ALOGW("Device not opened!");
        return INVALID_OPERATION;
    }

    if (close(fd) < 0) {
        ALOGE("Close video device failed: %s", strerror(errno));
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t CameraDriver::v4l2_capture_querycap(int fd, struct v4l2_capability *cap)
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;

    ret = ioctl(fd, VIDIOC_QUERYCAP, cap);

    if (ret < 0) {
        ALOGE("VIDIOC_QUERYCAP returned: %d (%s)", ret, strerror(errno));
        return ret;
    }

    if (!(cap->capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ALOGE("No capture devices");
        return -1;
    }

    if (!(cap->capabilities & V4L2_CAP_STREAMING)) {
        ALOGE("Is not a video streaming device");
        return -1;
    }

    LOG1( "driver:      '%s'", cap->driver);
    LOG1( "card:        '%s'", cap->card);
    LOG1( "bus_info:      '%s'", cap->bus_info);
    LOG1( "version:      %x", cap->version);
    LOG1( "capabilities:      %x", cap->capabilities);

    return ret;
}

int CameraDriver::set_capture_mode(Mode deviceMode)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_streamparm parm;

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.capturemode = deviceMode;
    LOG1("%s !! camID %d fd %d", __FUNCTION__, mCameraId, mCameraSensor[mCameraId]->fd);
    if (ioctl(mCameraSensor[mCameraId]->fd, VIDIOC_S_PARM, &parm) < 0) {
        ALOGE("error %s", strerror(errno));
        return -1;
    }

    return 0;
}

int CameraDriver::v4l2_capture_try_format(int fd, int *w, int *h)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_format v4l2_fmt;
    CLEAR(v4l2_fmt);

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_fmt.fmt.pix.width = *w;
    v4l2_fmt.fmt.pix.height = *h;
    v4l2_fmt.fmt.pix.pixelformat = mFormat;
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    ret = ioctl(fd, VIDIOC_TRY_FMT, &v4l2_fmt);
    if (ret < 0) {
        ALOGE("VIDIOC_TRY_FMT returned: %d (%s)", ret, strerror(errno));
        return -1;
    }

    *w = v4l2_fmt.fmt.pix.width;
    *h = v4l2_fmt.fmt.pix.height;

    return 0;
}

status_t CameraDriver::getPreviewFrame(CameraBuffer **buff)
{
    LOG2("@%s", __FUNCTION__);

    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return dequeueBuffer(buff);
}

status_t CameraDriver::putPreviewFrame(CameraBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return queueBuffer(buff);;
}

status_t CameraDriver::getRecordingFrame(CameraBuffer **buff, nsecs_t *timestamp)
{
    LOG2("@%s", __FUNCTION__);

    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return dequeueBuffer(buff, timestamp);
}

status_t CameraDriver::putRecordingFrame(CameraBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return queueBuffer(buff);;
}

status_t CameraDriver::getSnapshot(CameraBuffer **buff)
{
    LOG2("@%s", __FUNCTION__);

    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return dequeueBuffer(buff);
}

status_t CameraDriver::putSnapshot(CameraBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    return queueBuffer(buff);;
}

status_t CameraDriver::getThumbnail(CameraBuffer **buff)
{
    LOG1("@%s", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t CameraDriver::putThumbnail(CameraBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    return INVALID_OPERATION;
}

bool CameraDriver::dataAvailable()
{
    return mBufferPool.numBuffersQueued > 0;
}

bool CameraDriver::isBufferValid(const CameraBuffer* buffer) const
{
    return buffer->mDriverPrivate == this->mSessionId;
}

////////////////////////////////////////////////////////////////////
//                          PRIVATE METHODS
////////////////////////////////////////////////////////////////////

int CameraDriver::getNumberOfCameras()
{
    LOG1("@%s", __FUNCTION__);
    return CameraDriver::enumerateCameras();
}

status_t CameraDriver::getCameraInfo(int cameraId, camera_info *cameraInfo)
{
    LOG1("@%s: cameraId = %d", __FUNCTION__, cameraId);
    if (cameraId >= MAX_CAMERAS || cameraId < 0 || mCameraSensor[cameraId] == 0)
        return BAD_VALUE;

    memcpy(cameraInfo, &mCameraSensor[cameraId]->info, sizeof(camera_info));
    LOG1("%s: cameraId = %d, %s, %d", __FUNCTION__, cameraId,
            cameraInfo->facing == CAMERA_FACING_FRONT ? "front" : "back",
            cameraInfo->orientation);
    return NO_ERROR;
}

// Prop definitions.
static const char *PROP_PREFIX = "ro.camera";
static const char *PROP_NUMBER = "number";
static const char *PROP_DEVNAME = "devname";
static const char *PROP_FACING = "facing";
static const char *PROP_ORIENTATION = "orientation";
static const char *PROP_FACING_FRONT = "front";
static const char *PROP_FACING_BACK = "back";

// This function could be called from HAL's get_number_of_cameras() interface
// even before any CameraDriver's instance is created. For ANY errors, we will
// return 0 cameras detected to Android.
int CameraDriver::enumerateCameras(){
    int terminated = 0;
    static struct CameraSensor *newDev;
    int claimed;
    LOG1("@%s", __FUNCTION__);

    Mutex::Autolock _l(mCameraSensorLock);

    // clean up old enumeration.
    cleanupCameras();

    //start a new enumeration for all cameras

    char propKey[PROPERTY_KEY_MAX];
    char propVal[PROPERTY_VALUE_MAX];

    // get total number of cameras
    snprintf(propKey, sizeof(propKey), "%s.%s", PROP_PREFIX, PROP_NUMBER);
    if (0 == property_get(propKey, propVal, 0)) {
        ALOGE("%s: Failed to get number of cameras from prop.", __FUNCTION__);
        goto abort;
    }

    claimed = atoi(propVal);

    if (claimed < 0) {
        ALOGE("%s: Invalid Claimed (%d) camera(s), abort.", __FUNCTION__, claimed);
        goto abort;
    }

    if (claimed > MAX_CAMERAS) {
        ALOGD("%s: Claimed (%d) camera(s), but we only support up to (%d) camera(s)",
                __FUNCTION__, claimed, MAX_CAMERAS);
        claimed = MAX_CAMERAS;
    }

    for (int i = 0; i < claimed; i++) {
        newDev = new CameraSensor;
        if (!newDev) {
            ALOGE("%s: No mem for enumeration, abort.", __FUNCTION__);
            goto abort;
        }
        memset(newDev, 0, sizeof(struct CameraSensor));

        newDev->devName = new char[PROPERTY_VALUE_MAX];
        if (!newDev->devName) {
            ALOGE("%s: No mem for dev name, abort.", __FUNCTION__);
            goto abort;
        }

        // each camera device must have a name
        snprintf(propKey, sizeof(propKey), "%s.%d.%s", PROP_PREFIX, i, PROP_DEVNAME);
        if (0 == property_get(propKey, newDev->devName, 0)) {
            ALOGE("%s: Failed to get name of camera %d from prop, abort.",
                    __FUNCTION__, i);
            goto abort;
        }

        // Setup facing info
        snprintf(propKey, sizeof(propKey), "%s.%d.%s", PROP_PREFIX, i, PROP_FACING);
        if (0 == property_get(propKey, propVal, 0)) {
            ALOGE("%s: Failed to get facing of camera %d from prop, abort.",
                    __FUNCTION__, i);
            goto abort;
        }
        if (!strncmp(PROP_FACING_FRONT, propVal, strlen(PROP_FACING_FRONT))) {
            newDev->info.facing = CAMERA_FACING_FRONT;
        }
        else if (!strncmp(PROP_FACING_BACK, propVal, strlen(PROP_FACING_BACK))) {
            newDev->info.facing = CAMERA_FACING_BACK;
        }
        else {
            ALOGE("%s: Invalid facing of camera %d from prop, abort.",
                    __FUNCTION__, i);
            goto abort;
        }

        // setup orientation
        snprintf(propKey, sizeof(propKey), "%s.%d.%s",
                PROP_PREFIX, i, PROP_ORIENTATION);

        if (0 == property_get(propKey, propVal, 0) ||
                (newDev->info.orientation = atoi(propVal)) < 0) {
            ALOGE("%s: Invalid orientation of camera %d from prop, abort.",
                    __FUNCTION__, i);
            goto abort;
        }

        newDev->fd = -1;

        //It seems we get all info of a new camera
        ALOGD("%s: Detected camera (%d) %s %s %d",
                __FUNCTION__, i, newDev->devName,
                newDev->info.facing == CAMERA_FACING_FRONT ? "front" : "back",
                newDev->info.orientation);
        mCameraSensor[i] = newDev;
        numCameras++;
    }

    return numCameras;

abort:
    ALOGE("%s: Terminate camera enumeration !!", __FUNCTION__);
    cleanupCameras();
    //something wrong, further cleaning job
    if (newDev) {
        if (newDev->devName) {
            delete []newDev->devName;
            newDev->devName = 0;
        }
        delete newDev;
        newDev = 0;
    }
    return 0;
}

// Clean up camera  enumeration info
// Caller needs to take care syncing
void CameraDriver::cleanupCameras(){
    // clean up old enumeration
    LOG1("@%s: clean up", __FUNCTION__);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (mCameraSensor[i]) {
            LOG1("@%s: found old camera (%d)", __FUNCTION__, i);
            struct CameraSensor *cam = mCameraSensor[i];
            if (cam->fd > 0) {
                // Should we release buffers?
                close(cam->fd);
                cam->fd = -1;
            }
            if (cam->devName) {
                delete []cam->devName;
                cam->devName = 0;
            }
            delete cam;
            mCameraSensor[i] = 0;
        }
    }
    numCameras = 0;
}

status_t CameraDriver::autoFocus()
{
    return INVALID_OPERATION;
}

status_t CameraDriver::cancelAutoFocus()
{
    return INVALID_OPERATION;
}

status_t CameraDriver::setEffect(Effect effect)
{
    if (effect != EFFECT_NONE) {
        ALOGE("invalid color effect");
        return BAD_VALUE;
    }

    // Do nothing. EFFECT_NONE is all we support.

    return NO_ERROR;;
}

status_t CameraDriver::setFlashMode(FlashMode flashMode)
{
    if (flashMode != FLASH_MODE_OFF) {
        ALOGE("invalid flash mode");
        return BAD_VALUE;
    }

    // Do nothing. FLASH_MODE_OFF is all we support.

    return NO_ERROR;;
}

status_t CameraDriver::setSceneMode(SceneMode sceneMode)
{
    if (sceneMode != SCENE_MODE_AUTO) {
        ALOGE("invalid scene mode");
        return BAD_VALUE;
    }

    // Do nothing. SCENE_MODE_AUTO is all we support.

    return NO_ERROR;;
}

status_t CameraDriver::setFocusMode(FocusMode focusMode, CameraWindow *windows, int numWindows)
{
    if (focusMode != FOCUS_MODE_FIXED) {
        ALOGE("invalid focus mode");
        return BAD_VALUE;
    }

    if (windows != NULL || numWindows != 0) {
        ALOGE("focus windows not supported");
        return INVALID_OPERATION;
    }

    // Do nothing. FOCUS_MODE_FIXED is all we support.

    return NO_ERROR;
}

status_t CameraDriver::setWhiteBalanceMode(WhiteBalanceMode wbMode)
{
    if (wbMode != WHITE_BALANCE_AUTO) {
        ALOGE("invalid white balance");
        return BAD_VALUE;
    }

    // Do nothing. WHITE_BALANCE_AUTO is all we support.

    return NO_ERROR;;
}

status_t CameraDriver::setAeLock(bool lock)
{
    ALOGE("ae lock not supported");
    return INVALID_OPERATION;
}

status_t CameraDriver::setAwbLock(bool lock)
{
    ALOGE("awb lock not supported");
    return INVALID_OPERATION;
}

status_t CameraDriver::setMeteringAreas(CameraWindow *windows, int numWindows)
{
    ALOGE("metering not supported");
    return INVALID_OPERATION;
}

} // namespace android
