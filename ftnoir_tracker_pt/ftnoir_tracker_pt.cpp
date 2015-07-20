/* Copyright (c) 2012 Patrick Ruoff
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "ftnoir_tracker_pt.h"
#include <QHBoxLayout>
#include <cmath>
#include <QDebug>
#include <QFile>
#include <QCoreApplication>
#include "opentrack/camera-names.hpp"

using namespace std;
using namespace cv;

//#define PT_PERF_LOG	//log performance

//-----------------------------------------------------------------------------
Tracker_PT::Tracker_PT()
    : mutex(QMutex::Recursive),
      commands(0),
	  video_widget(NULL),
	  video_frame(NULL),
      ever_success(false)
{
    connect(s.b.get(), SIGNAL(saving()), this, SLOT(apply_settings()));
}

Tracker_PT::~Tracker_PT()
{
	set_command(ABORT);
	wait();
    delete video_widget;
    video_widget = NULL;
    if (video_frame->layout()) delete video_frame->layout();
    camera.stop();
}

void Tracker_PT::set_command(Command command)
{
    //QMutexLocker lock(&mutex);
	commands |= command;
}

void Tracker_PT::reset_command(Command command)
{
    //QMutexLocker lock(&mutex);
	commands &= ~command;
}

float Tracker_PT::get_focal_length()
{
    static constexpr float pi = 3.1415926;
    float fov_;
    switch (s.fov)
    {
    default:
    case 0:
        fov_ = 56;
        break;
    case 1:
        fov_ = 75;
        break;
    }

    const float diag_fov = static_cast<int>(fov_) * pi / 180.f;
    QMutexLocker l(&camera_mtx);
    CamInfo info = camera.get_info();
    const int w = info.res_x, h = info.res_y;
    const double diag = sqrt(w * w + h * h)/w;
    const double fov = 2.*atan(tan(diag_fov/2.0)/sqrt(1. + diag*diag));
    return w*.5 / tan(.5 * fov);
}

void Tracker_PT::run()
{
#ifdef PT_PERF_LOG
	QFile log_file(QCoreApplication::applicationDirPath() + "/PointTrackerPerformance.txt");
	if (!log_file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
	QTextStream log_stream(&log_file);
#endif

    apply_settings();
    
    while((commands & ABORT) == 0)
    {
        const double dt = time.elapsed() * 1e-9;
        time.start();
        cv::Mat frame;
        bool new_frame;

        {
            QMutexLocker l(&camera_mtx);
            new_frame = camera.get_frame(dt, &frame);
        }

        if (new_frame && !frame.empty())
        {
            QMutexLocker lock(&mutex);

            std::vector<cv::Vec2f> points = point_extractor.extract_points(frame);
            
            bool success = points.size() == PointModel::N_POINTS;
            
            ever_success |= success;
            
            if (success)
            {
                point_tracker.track(points, PointModel(s), get_focal_length(), s.dynamic_pose, s.init_phase_timeout);
            }
            
            {
                Affine X_CM = pose();
                Affine X_MH(Matx33f::eye(), cv::Vec3f(s.t_MH_x, s.t_MH_y, s.t_MH_z)); // just copy pasted these lines from below
                Affine X_GH = X_CM * X_MH;
                cv::Vec3f p = X_GH.t; // head (center?) position in global space
                float fx = get_focal_length();
                cv::Vec2f p_(p[0] / p[2] * fx, p[1] / p[2] * fx);  // projected to screen
                points.push_back(p_);
            }
            
            for (unsigned i = 0; i < points.size(); i++)
            {
                auto& p = points[i];
                cv::Scalar color(0, 255, 0);
                if (i == points.size()-1)
                    color = cv::Scalar(0, 0, 255);
                cv::line(frame,
                         cv::Point(p[0] - 20, p[1]),
                         cv::Point(p[0] + 20, p[1]),
                         color,
                         4);
                cv::line(frame,
                         cv::Point(p[0], p[1] - 20),
                         cv::Point(p[0], p[1] + 20),
                         color,
                         4);
            }
            
            video_widget->update_image(frame);
        }
#ifdef PT_PERF_LOG
        log_stream<<"dt: "<<dt;
        if (!frame.empty()) log_stream<<" fps: "<<camera.get_info().fps;
        log_stream<<"\n";
#endif
    }
    qDebug()<<"Tracker:: Thread stopping";
}

void Tracker_PT::apply_settings()
{
    qDebug()<<"Tracker:: Applying settings";
    QMutexLocker l(&camera_mtx);
    camera.stop();
    camera.set_device_index(camera_name_to_index("PS3Eye Camera"));
    int res_x, res_y, cam_fps;
    switch (s.camera_mode)
    {
    default:
    case 0:
        res_x = 640;
        res_y = 480;
        cam_fps = 75;
        break;
    case 1:
        res_x = 640;
        res_y = 480;
        cam_fps = 60;
        break;
    case 2:
        res_x = 320;
        res_y = 240;
        cam_fps = 189;
        break;
    case 3:
        res_x = 320;
        res_y = 240;
        cam_fps = 120;
        break;
    }

    camera.set_res(res_x, res_y);
    camera.set_fps(cam_fps);
    camera.start();
    qDebug()<<"Tracker::apply ends";
}

void Tracker_PT::start_tracker(QFrame *parent_window)
{
    this->video_frame = parent_window;
    video_frame->setAttribute(Qt::WA_NativeWindow);
    video_frame->show();
    video_widget = new PTVideoWidget(video_frame);
    QHBoxLayout* video_layout = new QHBoxLayout(parent_window);
    video_layout->setContentsMargins(0, 0, 0, 0);
    video_layout->addWidget(video_widget);
    video_frame->setLayout(video_layout);
    video_widget->resize(video_frame->width(), video_frame->height());
    start();
}

void Tracker_PT::data(double *data)
{
    if (ever_success)
    {
        Affine X_CM = pose();
    
        Affine X_MH(Matx33f::eye(), cv::Vec3f(s.t_MH_x, s.t_MH_y, s.t_MH_z));
        Affine X_GH = X_CM * X_MH;
    
        Matx33f R = X_GH.R;
        Vec3f   t = X_GH.t;
    
        // translate rotation matrix from opengl (G) to roll-pitch-yaw (E) frame
        // -z -> x, y -> z, x -> -y
        Matx33f R_EG(0, 0,-1,
                    -1, 0, 0,
                     0, 1, 0);
        R = R_EG * R * R_EG.t();
    
        // extract rotation angles
        float alpha, beta, gamma;
        beta  = atan2( -R(2,0), sqrt(R(2,1)*R(2,1) + R(2,2)*R(2,2)) );
        alpha = atan2( R(1,0), R(0,0));
        gamma = atan2( R(2,1), R(2,2));
    
        // extract rotation angles
        data[Yaw] = rad2deg * alpha;
        data[Pitch] = -rad2deg * beta;
        data[Roll] = rad2deg * gamma;
        // get translation(s)
        data[TX] = t[0] / 10.0;	// convert to cm
        data[TY] = t[1] / 10.0;
        data[TZ] = t[2] / 10.0;
    }
}
