#ifndef FRAME_WORKER_H
#define FRAME_WORKER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QVector>
#include "take_object.hpp"
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/shared_ptr.hpp>
class frameWorker : public QObject
{
    Q_OBJECT
public:
    explicit frameWorker(QObject *parent = 0);
    virtual ~frameWorker();
    uint16_t * getImagePtr();
    uint16_t * getRawPtr();

    float * getDSF();
    float * getStdDevData();

    float * getVerticalMean();
    float * getHorizontalMean();
    float * getRealFFTSquared();

    uint32_t * getHistogramData();
    std::vector<float> *getHistogramBins();
    unsigned int getFrameHeight();
    unsigned int getDataHeight();
    unsigned int getFrameWidth();

    bool dsfMaskCollected();

    camera_t camera_type();

    double histoDataMax;

    QMutex vector_mutex;
    QVector<double> histo_data_vec;
    QVector<double> rfft_data_vec;

    unsigned int old_save_framenum;
signals:
    void newFrameAvailable();
    void std_dev_ready();
    void savingFrameNumChanged(unsigned int n);
public slots:
    void captureFrames();
    void startCapturingDSFMask();
    void finishCapturingDSFMask();
    void loadDSFMask(QString);

    void startSavingRawData(unsigned int framenum,QString name);
    void stopSavingRawData();


    void setStdDev_N(int newN);
    unsigned int getFrameHeight();
    unsigned int getFrameWidth();
    unsigned int getDataHeight();
private:
    take_object to;

    float * rfft_data;
    uint32_t * histo_data;
    void updateFFTVector();
    void updateHistogramVector();

    unsigned int frHeight;
    unsigned int frWidth;
    unsigned int dataHeight;

    float * raw_data;
    float * image_data;
    float * dsf_data;
    float * std_dev_data;



};

#endif // FRAME_WORKER_H
