//  Powiter
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
*Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012. 
*contact: immarespond at gmail dot com
*
*/

#include "Reader.h"

#include <cassert>
#include <sstream>

#include <QtCore/QMutex>
#include <QtGui/QImage>
#include <QtConcurrentRun>

#include "Global/Macros.h"
#include "Global/AppManager.h"
#include "Global/LibraryBinary.h"


#include "Engine/Node.h"
#include "Engine/MemoryFile.h"
#include "Engine/VideoEngine.h"
#include "Engine/Model.h"
#include "Engine/Settings.h"
#include "Engine/Box.h"
#include "Engine/Format.h"
#include "Engine/FrameEntry.h"
#include "Engine/ViewerNode.h"
#include "Engine/Knob.h"
#include "Engine/ImageInfo.h"
#include "Engine/Lut.h"

#include "Readers/ReadFfmpeg_deprecated.h"
#include "Readers/ReadExr.h"
#include "Readers/ReadQt.h"
#include "Readers/Read.h"

#include "Writers/Writer.h"

using namespace Powiter;
using namespace std;


Reader::Reader(Model* model)
: Node(model)
, _buffer()
, _fileKnob(0)
, _previewWatcher(new QFutureWatcher<void>())
{
    QObject::connect(_previewWatcher.get(),SIGNAL(finished()),this,SLOT(notifyGuiPreviewChanged()));
}

Reader::~Reader(){
    _buffer.clear();
}

std::string Reader::className() const {
    return "Reader";
}

std::string Reader::description() const {
    return "The reader node can read image files sequences.";
}

void Reader::initKnobs(){
    std::string desc("File");
    _fileKnob = dynamic_cast<File_Knob*>(appPTR->getKnobFactory().createKnob("InputFile", this, desc));
    QObject::connect(_fileKnob, SIGNAL(valueChangedByUser()), this, SLOT(showFilePreview()));
    QObject::connect(_fileKnob, SIGNAL(frameRangeChanged(int,int)), this, SLOT(onFrameRangeChanged(int,int)));
    assert(_fileKnob);
}

void Reader::onFrameRangeChanged(int first,int last){
    _frameRange.first = first;
    _frameRange.second = last;
}
boost::shared_ptr<Read> Reader::decoderForFileType(const QString& fileName){
    QString extension;
    for (int i = fileName.size() - 1; i >= 0; --i) {
        QChar c = fileName.at(i);
        if(c != QChar('.'))
            extension.prepend(c);
        else
            break;
    }

    Powiter::LibraryBinary* decoder = Settings::getPowiterCurrentSettings()->_readersSettings.decoderForFiletype(extension.toStdString());
    if (!decoder) {
        string err("Couldn't find an appropriate decoder for this filetype");
        err.append(extension.toStdString());
        throw std::invalid_argument(err);
    }
    
    pair<bool,ReadBuilder> func = decoder->findFunction<ReadBuilder>("BuildRead");
    if (func.first) {
       return  boost::shared_ptr<Read>(func.second(this));
    } else {
        string err("Failed to create the decoder for");
        err.append(getName());
        err.append(",something is wrong in the plugin.");
        throw std::invalid_argument(err);
    }
    return boost::shared_ptr<Read>();
}

Powiter::Status Reader::getRegionOfDefinition(SequenceTime time,Box2D* rod){
    QString filename = _fileKnob->getRandomFrameName(time);
    
    /*Locking any other thread: we want only 1 thread to create the descriptor*/
    QMutexLocker lock(&_lock);
    boost::shared_ptr<Read> found = _buffer.get(filename.toStdString());
    if (found) {
        *rod = found->readerInfo().getDataWindow();
    }else{
        boost::shared_ptr<Read> desc;
        try{
            desc = decodeHeader(filename);
        }catch(const std::invalid_argument& e){
            cout << e.what() << endl;
            return StatFailed;
        }
        if(desc){
            *rod =  desc->readerInfo().getDataWindow();
        }else{
            return StatFailed;
        }
    }
    return StatOK;
}

boost::shared_ptr<Read> Reader::decodeHeader(const QString& filename){
    /*the read handle used to decode the frame*/
    boost::shared_ptr<Read> decoderReadHandle;
    try{
        decoderReadHandle = decoderForFileType(filename);
    }catch(const std::invalid_argument& e){
        throw e;
    }
    assert(decoderReadHandle);
    Status st = decoderReadHandle->readHeader(filename);
    if(st == StatFailed){
        return boost::shared_ptr<Read>();
    }
    decoderReadHandle->initializeColorSpace();
    _buffer.insert(filename.toStdString(),decoderReadHandle);
    return decoderReadHandle;
}

static float clamp(float v, float min = 0.f, float max= 1.f){
    if(v > max) v = max;
    if(v < min) v = min;
    return v;
}

void Reader::showFilePreview(){
    _previewWatcher->setFuture(QtConcurrent::run(this,&Reader::showFilePreviewInternal));
}

void Reader::showFilePreviewInternal(){
    SequenceTime time = firstFrame();
    QString filename = _fileKnob->getRandomFrameName(time);
    boost::shared_ptr<Read> desc;
    {
        QMutexLocker lock(&_lock);
        desc = _buffer.get(filename.toStdString());
        if(!desc){
            try{
                desc = decodeHeader(filename);
            }catch(const std::invalid_argument& e){
                cout << e.what() << endl;
                return;
            }
        }
    }
    const Box2D& rod = desc->readerInfo().getDataWindow();
    int h,w;
    rod.height() < POWITER_PREVIEW_HEIGHT ? h = rod.height() : h = POWITER_PREVIEW_HEIGHT;
    rod.width() < POWITER_PREVIEW_WIDTH ? w = rod.width() : w = POWITER_PREVIEW_WIDTH;
    float yZoomFactor = (float)h/(float)rod.height();
    float xZoomFactor = (float)w/(float)rod.width();
    QImage img(w,h,QImage::Format_ARGB32);
    for (int i=0; i< h; ++i) {
        double y = i*1.f/yZoomFactor;
        int nearestY = (int)(y+0.5);
        
        /*get calls render and also caches the row!*/
        boost::shared_ptr<const Row> row = get(time, nearestY, rod.left(), rod.right(), desc->readerInfo().getChannels());
        
        QRgb *dst_pixels = (QRgb *) img.scanLine(h-1-i);
        const float* red = row->begin(Channel_red);
        const float* green = row->begin(Channel_green);
        const float* blue = row->begin(Channel_blue);
        const float* alpha = row->begin(Channel_alpha);
        for (int j=0; j<w; ++j) {
            double x = j*1.f/xZoomFactor;
            int nearestX = (int)(x+0.5);
            float r = red ? clamp(Color::linearrgb_to_srgb(red[nearestX])) : 0.f;
            float g = green ? clamp(Color::linearrgb_to_srgb(green[nearestX])) : 0.f;
            float b = blue ? clamp(Color::linearrgb_to_srgb(blue[nearestX])) : 0.f;
            float a = alpha ? clamp(alpha[nearestX]) : 1.f;
            QColor c(r*255,g*255,b*255,a*255);
            dst_pixels[j] = qRgba(r*255,g*255,b*255,a*255);
        }
        
    }
    _preview = img;

}

void Reader::render(SequenceTime time,Row* out){
    QString filename = _fileKnob->getRandomFrameName(time);
    boost::shared_ptr<Read> found;
    {
        /*This section is critical: if we don't find it in the buffer we want only 1 thread to re-create the descriptor.
         */
        QMutexLocker lock(&_lock);
        found = _buffer.get(filename.toStdString());
        if(!found){
#ifdef POWITER_DEBUG
            cout << "WARNING: Buffer does not contains the header for frame " << filename.toStdString()
            <<". re-decoding header...(" << getName() << "). Ignore this is the call was made by showFilePreview()" << endl;
#endif
            try{
                found = decodeHeader(filename);
            }catch(const std::invalid_argument& e){
                cout << e.what() << endl;
            }
        }
    }
    found->render(time,out);
}


int Reader::firstFrame(){
    return _fileKnob->firstFrame();
}
int Reader::lastFrame(){
    return _fileKnob->lastFrame();
}
int Reader::nearestFrame(int f){
    return _fileKnob->nearestFrame(f);
}

const QString Reader::getRandomFrameName(int f) const{
    return _fileKnob->getRandomFrameName(f);
}


QImage Reader::getPreview(int /*width*/,int /*height*/){
    return _preview;
}


bool Reader::hasFrames() const{
    return _fileKnob->frameCount() > 0;
}

