
#include "ofxOrbbecCamera.h"

//Static functions 
std::shared_ptr <ob::Context> ofxOrbbecCamera::ofxOrbbecCamera::ctx;
std::shared_ptr<ob::Context> & ofxOrbbecCamera::getContext(){
    if(!ctx){
        // don't log to file 
        ob::Context::setLoggerToFile(OB_LOG_SEVERITY_OFF, "log.txt"); 
        ctx = make_shared<ob::Context>(); 
    }
    return ctx; 
}

std::vector < std::shared_ptr<ob::DeviceInfo> > ofxOrbbecCamera::getDeviceList(){
    std::vector<std::shared_ptr<ob::DeviceInfo> > dInfo; 

    auto tCtx = ofxOrbbecCamera::getContext(); 

    // Query the list of connected devices
        auto devList = tCtx->queryDeviceList();
    
    // Get the number of connected devices
    int devCount = devList->deviceCount();

    // traverse the device list and create a pipe
    for(int i = 0; i < devCount; i++) {
        auto dev  = devList->getDevice(i);
        auto info = dev->getDeviceInfo();

        ofLogNotice("ofxOrbbecCamera::getDeviceList()") << "["<< i <<"] device is " << info->name() << " serial: " << info->serialNumber() << std::endl; 

        dInfo.push_back(info);
    }

    return dInfo; 
}

//Class functions 
ofxOrbbecCamera::~ofxOrbbecCamera(){
    close();
    #ifdef OFXORBBEC_DECODE_H264_H265
        if(bInitOneTime){
            avcodec_close(codecContext264);
            av_free(codecContext264);

            avcodec_close(codecContext265);
            av_free(codecContext265);
        }
    #endif 
}

void ofxOrbbecCamera::close(){
    clear();
}

void ofxOrbbecCamera::clear(){
    try{
        if( mPipe ){
            //doing this to prevent issues on app exit 
            auto frameSet = mPipe->waitForFrames(20);
            mPipe->stop();
            //doing this to prevent issues on app exit 
            ofSleepMillis(20);
        }
        mPipe.reset(); 
        pointCloud.reset();
    }catch(ob::Error &e) {
        std::cerr << "function:" << e.getName() << "\nargs:" << e.getArgs() << "\nmessage:" << e.getMessage() << "\ntype:" << e.getExceptionType() << std::endl;
    }

    mCurrentSettings = ofxOrbbec::Settings();
    bNewFrameColor = bNewFrameDepth = bNewFrameIR = false; 
}

bool ofxOrbbecCamera::open(ofxOrbbec::Settings aSettings){
    clear(); 

    auto tCtx = ofxOrbbecCamera::getContext(); 

    std::shared_ptr<ob::Device> device;

    //need depth frames for point cloud
    if( aSettings.bPointCloud && !aSettings.bDepth ){
        aSettings.bDepth = true; 
    }

    mCurrentSettings = aSettings; 

    if( aSettings.ip != ""){
        try{
            device = tCtx->createNetDevice(aSettings.ip.c_str(), 8090);
        }catch(ob::Error &e) {
            std::cerr << "function:" << e.getName() << "\nargs:" << e.getArgs() << "\nmessage:" << e.getMessage() << "\ntype:" << e.getExceptionType() << std::endl;
        }
        if(!device){
            return false; 
        }
    }else{

        // Query the list of connected devices
         auto devList = tCtx->queryDeviceList();
        
        // Get the number of connected devices
        int devCount = devList->deviceCount();

        bool openWithSerial = aSettings.deviceSerial != "";

        // traverse the device list and create a pipe
        for(int i = 0; i < devCount; i++) {
            // Get the device and create the pipeline
            auto dev  = devList->getDevice(i);
            auto info = dev->getDeviceInfo();

            std::cout << "["<< i <<"] device is " << info->name() << " serial: " << info->serialNumber() << std::endl; 

            if( openWithSerial ){
                string serialStr(info->serialNumber()); 
                if( aSettings.deviceSerial == serialStr ){
                    device = dev;
                    break; 
                }
            }else{
                if( aSettings.deviceID == i ){
                    device = dev; 
                    break; 
                }
            }
        }

    }    

    if( device ){
        // pass in device to create pipeline
        mPipe = std::make_shared<ob::Pipeline>(device);
    
        if( mPipe ){

             // Create Config for configuring Pipeline work
            std::shared_ptr<ob::Config> config = std::make_shared<ob::Config>();

            if( aSettings.bDepth ){
                // Get the depth camera configuration list
                auto depthProfileList = mPipe->getStreamProfileList(OB_SENSOR_DEPTH);

                shared_ptr<ob::StreamProfile> depthProfile;

                if( mCurrentSettings.depthFrameSize.requestWidth > 0){
                    try {
                        auto requestType = mCurrentSettings.depthFrameSize;

                        // Find the corresponding profile according to the specified format
                        depthProfile = depthProfileList->getVideoStreamProfile(requestType.requestWidth, requestType.requestHeight, requestType.format, requestType.frameRate);
                    }
                    catch(ob::Error &e) {
                        ofLogWarning("ofxOrbbecCamera::open") << " couldn't open depth with requested dimensions / format - using default "; 
                        depthProfile = depthProfileList->getProfile(0);
                    }

                }else{  
                    depthProfile = depthProfileList->getProfile(0);
                }
                
                // enable depth stream
                config->enableStream(depthProfile);
            }

            if( aSettings.bColor ){
                // Get the color camera configuration list
                auto colorProfileList = mPipe->getStreamProfileList(OB_SENSOR_COLOR);
                shared_ptr<ob::StreamProfile> colorProfile;

                if( mCurrentSettings.colorFrameSize.requestWidth > 0){
                    try {
                        auto requestType = mCurrentSettings.colorFrameSize;

                        // Find the corresponding profile according to the specified format
                        colorProfile = colorProfileList->getVideoStreamProfile(requestType.requestWidth, requestType.requestHeight, requestType.format, requestType.frameRate);
                    }
                    catch(ob::Error &e) {
                        ofLogWarning("ofxOrbbecCamera::open") << " couldn't open color with requested dimensions / format - using default "; 
                        colorProfile = colorProfileList->getProfile(0);
                    }

                }else{  
                    colorProfile = colorProfileList->getProfile(0);
                }
                
                // enable color stream
                config->enableStream(colorProfile);
            }

            if( aSettings.bPointCloud ){
                if( aSettings.bColor && aSettings.bPointCloudRGB ){
                    config->setAlignMode(ALIGN_D2C_HW_MODE);
                }else{
                    config->setAlignMode(ALIGN_DISABLE);
                }
            }
            

            // Pass in the configuration and start the pipeline
            mPipe->start(config);

            if( aSettings.bPointCloud || aSettings.bPointCloudRGB ){
                auto cameraParam = mPipe->getCameraParam();

                pointCloud = std::make_shared<ob::PointCloudFilter>();
                pointCloud->setCameraParam(cameraParam);
                if( aSettings.bPointCloudRGB ){
                    pointCloud->setCreatePointFormat(OB_FORMAT_RGB_POINT);
                }else{
                    pointCloud->setCreatePointFormat(OB_FORMAT_POINT);
                }
            }

            ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_ERROR);

        }else{
            return false; 
        }

    }

    return false; 
}

bool ofxOrbbecCamera::isConnected(){
	if( mPipe && mPipe->getDevice() ){
		return true;
	}
	return false;
}

ofPixels ofxOrbbecCamera::getDepthPixels(){
    return mDepthPixels;
}

ofFloatPixels ofxOrbbecCamera::getDepthPixelsF(){
    return mDepthPixelsF;
} 

ofPixels ofxOrbbecCamera::getColorPixels(){
    return mColorPixels;
}

vector <glm::vec3> ofxOrbbecCamera::getPointCloud(){
    return mPointCloudPts;
} 

ofMesh ofxOrbbecCamera::getPointCloudMesh(){
    return mPointCloudMesh;
}

void ofxOrbbecCamera::update(){

    bNewFrameDepth = false;
    bNewFrameColor = false; 
    bNewFrameIR = false; 

    if( mPipe ){
        //TODO: thread me 
        auto frameSet = mPipe->waitForFrames(20);
        if(frameSet) {
            
            if( mCurrentSettings.bDepth ){
                auto depthFrame = frameSet->getFrame(OB_FRAME_DEPTH);
                if(depthFrame) {
                    mDepthPixels = processFrame(depthFrame);

                    if( mCurrentSettings.bPointCloud && !mCurrentSettings.bPointCloudRGB ){
                        try {
                            std::shared_ptr<ob::Frame> frame = pointCloud->process(frameSet);
                            pointCloudToMesh(frame);
                        }
                        catch(std::exception &e) {
                            std::cout << "Get point cloud failed" << std::endl;
                        };
                    }

                    bNewFrameDepth = true; 
                }
            }

            if( mCurrentSettings.bColor ){
                auto colorFrame = frameSet->getFrame(OB_FRAME_COLOR);
                if(colorFrame) {
                    mColorPixels = processFrame(colorFrame);

                    if( mCurrentSettings.bPointCloudRGB ){
                        if(frameSet != nullptr && frameSet->depthFrame() != nullptr && frameSet->colorFrame() != nullptr) {
                            // point position value multiply depth value scale to convert uint to millimeter (for some devices, the default depth value uint is not
                            // millimeter)
                            auto depthValueScale = frameSet->depthFrame()->getValueScale();
                            pointCloud->setPositionDataScaled(depthValueScale);
                            try {
                                std::shared_ptr<ob::Frame> frame = pointCloud->process(frameSet);
                                pointCloudToMesh(frame, true);
                            }
                            catch(std::exception &e) {
                                std::cout << "Get point cloud failed" << std::endl;
                            }
                        }
                    }

                    //In case h264 and we can't decode - pixels will be empty 
                    if(mColorPixels.getWidth()){
                        bNewFrameColor = true; 
                    }

                }
            }

        }
    }
}
        
bool ofxOrbbecCamera::isFrameNew(){
    return bNewFrameColor || bNewFrameDepth || bNewFrameIR;
}

bool ofxOrbbecCamera::isFrameNewDepth(){
    return bNewFrameDepth;
}

bool ofxOrbbecCamera::isFrameNewColor(){
    return bNewFrameColor;
}


#ifdef OFXORBBEC_DECODE_H264_H265

void ofxOrbbecCamera::initH26XCodecs(){
    if( !bInitOneTime ){
        // Initialize FFmpeg libraries
        av_register_all();
        avcodec_register_all();
        bInitOneTime = true; 

        // Allocate an AVCodecContext and set its codec
        codec264 = avcodec_find_decoder(AV_CODEC_ID_H264);
        codecContext264 = avcodec_alloc_context3(codec264);
        avcodec_open2(codecContext264, codec264, NULL);

        // // Allocate an AVCodecContext and set its codec
        codec265 = avcodec_find_decoder(AV_CODEC_ID_H265);
        codecContext265 = avcodec_alloc_context3(codec265);
        avcodec_open2(codecContext265, codec265, NULL);
    }
}

ofPixels ofxOrbbecCamera::decodeH26XFrame(uint8_t * myData, int dataSize, bool bH264){
    initH26XCodecs();
    
    AVPacket packet; 
    av_init_packet(&packet);
    packet.data = myData;
    packet.size = dataSize;

    // Allocate an AVFrame for decoded data
    AVFrame* frame = av_frame_alloc();
    
    ofPixels pix;

    auto codecContext = codecContext264;
    if( !bH264 ){
        codecContext = codecContext265; 
    }

    int ret = avcodec_send_packet(codecContext, &packet);
    if (ret < 0) {
        cout << "Error sending a packet for decoding" << endl; 
        return pix; 
    }
 
    int frameDecoded = avcodec_receive_frame(codecContext, frame);

    if( frameDecoded == 0 ){

        // Allocate an AVFrame for RGB data
        AVFrame* rgbFrame = av_frame_alloc();

        rgbFrame->format = AV_PIX_FMT_RGB24; 
        rgbFrame->width = codecContext->width; 
        rgbFrame->height = codecContext->height; 

        av_frame_get_buffer(rgbFrame, 0);

        // Create a sws context for RGB conversion
        swsContext = sws_getCachedContext(swsContext, codecContext->width, codecContext->height, codecContext->pix_fmt,
            codecContext->width, codecContext->height, (AVPixelFormat)rgbFrame->format, SWS_BILINEAR, NULL, NULL, NULL);
        
        // Convert the decoded frame to RGB
        int result = sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, rgbFrame->data, rgbFrame->linesize);
        pix.setFromPixels((unsigned char * )rgbFrame->data[0], codecContext->width, codecContext->height, 3); 

        av_frame_free(&rgbFrame);
    }

    // Clean up and free allocated memory
    av_frame_free(&frame);

    return pix; 
}

#endif 


ofPixels ofxOrbbecCamera::processFrame(shared_ptr<ob::Frame> frame){

    ofPixels pix; 
    cv::Mat imuMat;
    cv::Mat rstMat;

    try{
        
        if( !frame ){
            return pix; 
        }

        if(frame->type() == OB_FRAME_COLOR) {
            auto videoFrame = frame->as<ob::VideoFrame>();
            switch(videoFrame->format()) {
            case OB_FORMAT_H264:

                #ifdef OFXORBBEC_DECODE_H264_H265 
                    pix = decodeH26XFrame((uint8_t*)videoFrame->data(), videoFrame->dataSize(), true);
                #else
                    ofLogError("ofxOrbbecCamera::processFrame") << " h264 / h265 not enabled. Define OFXORBBEC_DECODE_H264_H265 or set color format to OB_FORMAT_RGB " << endl;
                #endif  

            break; 
            case OB_FORMAT_H265:

                #ifdef OFXORBBEC_DECODE_H264_H265 
                    pix = decodeH26XFrame((uint8_t*)videoFrame->data(), videoFrame->dataSize(), false);
                #else
                    ofLogError("ofxOrbbecCamera::processFrame") << " h264 / h265 not enabled. Define OFXORBBEC_DECODE_H264_H265 or set color format to OB_FORMAT_RGB " << endl;
                #endif 

            break; 
            case OB_FORMAT_MJPG: {

#if !defined(TARGET_OSX) && !defined(TARGET_WIN32)
                cv::Mat rawMat(1, videoFrame->dataSize(), CV_8UC1, videoFrame->data());
                rstMat = cv::imdecode(rawMat, 1);
                cv::cvtColor(rstMat, rstMat, cv::COLOR_BGR2RGB);
#else
                ofLogError("ofxOrbbecCamera::processFrame") << " MJPG not supported - set color format to OB_FORMAT_RGB " << endl;
#endif

            } break;
            case OB_FORMAT_NV21: {
                cv::Mat rawMat(videoFrame->height() * 3 / 2, videoFrame->width(), CV_8UC1, videoFrame->data());
                cv::cvtColor(rawMat, rstMat, cv::COLOR_YUV2RGB_NV21);
            } break;
            case OB_FORMAT_YUYV:
            case OB_FORMAT_YUY2: {
                cv::Mat rawMat(videoFrame->height(), videoFrame->width(), CV_8UC2, videoFrame->data());
                cv::cvtColor(rawMat, rstMat, cv::COLOR_YUV2RGB);
            } break;
            case OB_FORMAT_RGB: {
                cv::Mat rawMat(videoFrame->height(), videoFrame->width(), CV_8UC3, videoFrame->data());
                rstMat = rawMat;
            } break;
            case OB_FORMAT_UYVY: {
                cv::Mat rawMat(videoFrame->height(), videoFrame->width(), CV_8UC2, videoFrame->data());
                cv::cvtColor(rawMat, rstMat, cv::COLOR_YUV2RGB_UYVY);
            } break;
            default:
                break;
            }
            if(!rstMat.empty()) {
                pix.setFromPixels(rstMat.ptr(), rstMat.cols, rstMat.rows, rstMat.channels());
            }
        }
        else if(frame->type() == OB_FRAME_DEPTH) {
            auto videoFrame = frame->as<ob::VideoFrame>();
            if(videoFrame->format() == OB_FORMAT_Y16) {
                cv::Mat cvtMat;
                cv::Mat rawMat = cv::Mat(videoFrame->height(), videoFrame->width(), CV_16UC1, videoFrame->data());
                // depth frame pixel value multiply scale to get distance in millimeter
                float scale = videoFrame->as<ob::DepthFrame>()->getValueScale();

                // threshold to 5.46m
                cv::threshold(rawMat, cvtMat, 5460.0f / scale, 0, cv::THRESH_TRUNC);
                cvtMat.convertTo(cvtMat, CV_8UC1, scale * 0.05);
                rstMat = cvtMat;//cv::applyColorMap(cvtMat, rstMat, cv::COLORMAP_JET);
            }
            if(!rstMat.empty()) {
                pix.setFromPixels(rstMat.ptr(), rstMat.cols, rstMat.rows, rstMat.channels());
            }
        }
        else if(frame->type() == OB_FRAME_IR || frame->type() == OB_FRAME_IR_LEFT || frame->type() == OB_FRAME_IR_RIGHT) {
            auto videoFrame = frame->as<ob::VideoFrame>();
            if(videoFrame->format() == OB_FORMAT_Y16) {
                cv::Mat cvtMat;
                cv::Mat rawMat = cv::Mat(videoFrame->height(), videoFrame->width(), CV_16UC1, videoFrame->data());
                rawMat.convertTo(cvtMat, CV_8UC1, 1.0 / 16.0f);
                rstMat = rawMat;//cv::cvtColor(cvtMat, rstMat, cv::COLOR_GRAY2RGB);
            }
            else if(videoFrame->format() == OB_FORMAT_Y8) {
                cv::Mat rawMat = cv::Mat(videoFrame->height(), videoFrame->width(), CV_8UC1, videoFrame->data());
                rstMat = rawMat;//cv::cvtColor(rawMat * 2, rstMat, cv::COLOR_GRAY2RGB);
            }
            else if(videoFrame->format() == OB_FORMAT_MJPG) {
#if !defined(TARGET_OSX) && !defined(TARGET_WIN32)
                cv::Mat rawMat(1, videoFrame->dataSize(), CV_8UC1, videoFrame->data());
                rstMat = cv::imdecode(rawMat, 1);
                rstMat = rawMat;//cv::cvtColor(rstMat * 2, rstMat, cv::COLOR_GRAY2RGB);
#else
                ofLogError("ofxOrbbecCamera::processFrame") << " MJPG not supported - set IR format to OB_FORMAT_Y16 or OB_FORMAT_Y8 " << endl;
#endif
            }
            if(!rstMat.empty()) {
                pix.setFromPixels(rstMat.ptr(), rstMat.cols, rstMat.rows, rstMat.channels());
            }
        }
    } catch(const cv::Exception& ex) {
        ofLogError("processFrame") << " OB_FORMAT not supported " << std::endl; 
    }
    return pix; 
}

void ofxOrbbecCamera::pointCloudToMesh(shared_ptr<ob::Frame> frame, bool bRGB){
    if( frame ){
        
        int pointsSize = 0;

        if(bRGB){
            pointsSize = frame->dataSize() / sizeof(OBColorPoint);
        }else{
            pointsSize = frame->dataSize() / sizeof(OBPoint);
        }

        mPointCloudMesh = ofMesh();  
        mPointCloudPts.clear();
        mPointCloudPts.reserve(pointsSize);

        mPointCloudMesh.setMode(OF_PRIMITIVE_POINTS);


        if( bRGB ){
            std::vector <ofFloatColor> tColors;
            tColors.reserve(pointsSize);

            OBColorPoint *point = (OBColorPoint *)frame->data();
            for(int i = 0; i < pointsSize; i++) {
                auto pt = glm::vec3(point->x, -point->y, -point->z);

                mPointCloudPts.push_back(pt);
                tColors.push_back(ofColor((int)point->r, (int)point->g, (int)point->b, 255));

                point++;
            }
            mPointCloudMesh.addColors(tColors);

        }else{
            OBPoint *point = (OBPoint *)frame->data();
            for(int i = 0; i < pointsSize; i++) {
                auto pt = glm::vec3(point->x, -point->y, -point->z);

                mPointCloudPts.push_back(pt);
                point++;
            }
        }

        mPointCloudMesh.addVertices(mPointCloudPts);
        mPointCloudMesh.setupIndicesAuto(); 
    }
}
