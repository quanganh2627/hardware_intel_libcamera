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

#ifndef ANDROID_LIBCAMERA_PICTURE_THREAD_H
#define ANDROID_LIBCAMERA_PICTURE_THREAD_H

#include <utils/threads.h>
#include <camera.h>
#include <camera/CameraParameters.h>
#include "MessageQueue.h"
#include "CameraCommon.h"
#include "JpegCompressor.h"
#include "JpegEncoder.h" // for EXIF

namespace android {

class Callbacks;

// callback for when Picture thread is done with yuv data
class ICallbackPicture {
public:
    ICallbackPicture() {}
    virtual ~ICallbackPicture() {}
    virtual void pictureDone(CameraBuffer *snapshotBuf, CameraBuffer *postviewBuf) = 0;
};

class PictureThread : public Thread {

// constructor destructor
public:
    PictureThread(ICallbackPicture *pictureDone);
    virtual ~PictureThread();

// Thread overrides
public:
    status_t requestExitAndWait();

// public types
public:

    struct Image {
        int format;
        int quality;
        int width;
        int height;
    };

    struct Config {
        Image picture;
        Image thumbnail;
        exif_attribute_t exif;
    };

// public methods
public:

    status_t encode(CameraBuffer *snaphotBuf, CameraBuffer *postviewBuf = NULL);
    void getDefaultParameters(CameraParameters *params);
    void setConfig(Config *config);
    status_t flushBuffers();

// private types
private:

    // thread message id's
    enum MessageId {

        MESSAGE_ID_EXIT = 0,            // call requestExitAndWait
        MESSAGE_ID_ENCODE,
        MESSAGE_ID_FLUSH,

        // max number of messages
        MESSAGE_ID_MAX
    };

    //
    // message data structures
    //

    struct MessageEncode {
        CameraBuffer snaphotBuf;
        CameraBuffer postviewBuf;
    };

    // union of all message data
    union MessageData {

        // MESSAGE_ID_ENCODE
        MessageEncode encode;
    };

    // message id and message data
    struct Message {
        MessageId id;
        MessageData data;
    };

// private methods
private:

    // thread message execution functions
    status_t handleMessageExit();
    status_t handleMessageEncode(MessageEncode *encode);
    status_t handleMessageFlush();

    // main message function
    status_t waitForAndExecuteMessage();

    status_t encodeToJpeg(CameraBuffer *mainBuf, CameraBuffer *thumbBuf, CameraBuffer *destBuf);

// inherited from Thread
private:
    virtual bool threadLoop();

// private data
private:

    JpegEncoder encoder; // for EXIF
    MessageQueue<Message, MessageId> mMessageQueue;
    bool mThreadRunning;
    ICallbackPicture *mPictureDoneCallback;
    Callbacks *mCallbacks;
    JpegCompressor compressor;
    CameraBuffer mExifBuf;
    CameraBuffer mOutBuf;

    Config mConfig;

// public data
public:

}; // class PictureThread

}; // namespace android

#endif // ANDROID_LIBCAMERA_PICTURE_THREAD_H
