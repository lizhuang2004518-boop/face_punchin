// ============================================================================
//  原始 demo 备份（用户原来的 main.cpp）
//  仅用于对照，不参与构建（CMakeLists 里没有把它加入 add_executable）。
//  新程序请见上一级目录的 main.cpp。
// ============================================================================

#include "opencv2/opencv.hpp"

#include <iostream>

using namespace cv;
using namespace std;
void drawText(Mat & image);
void processImage(Mat & image);
// 定义一个人脸检测类(FaceDetectorYN)对象，并加载对应的模型(智能指针)
Ptr< FaceDetectorYN > fd = FaceDetectorYN::create("../models/yunet(1).onnx", "", Size(320, 320));

int main()
{
    Mat image;
    Mat faces;//存储检测到的人脸信息
    VideoCapture capture;
    capture.open(0);//打开索引为0的摄像头
    int x, y, w, h;//人脸区域的左上角坐标、宽度、高度
    if(capture.isOpened())//检查摄像头是否打开
    {
        cout << "Capture is opened" << endl;
        for(;;)
        {
            capture >> image;//
            if(image.empty())
                break;
            //转化为灰度图像
            //cvtColor(image, image, COLOR_BGR2GRAY);

            //processImage(image);
            //drawText(image);//在图像上绘制文字
            fd->setInputSize(image.size());
            fd->detect(image, faces);  // 通过深度神经网络分析出人脸区域

            // 遍历检测到的人脸区域
            for (int r = 0; r < faces.rows; r++)
            {
                // 人脸区域左上角坐标、宽度、高度、右眼坐标、左眼坐标、鼻子坐标、右嘴角坐标、左嘴角坐标、置信度
                x = faces.at<float>(r, 0);
                y = faces.at<float>(r, 1);
                w = faces.at<float>(r, 2);
                h = faces.at<float>(r, 3);

                // 绘制人脸区域矩形框
                rectangle(image, Point(x, y), Point(x + w, y + h), Scalar(0, 255, 0), 2);
            }

            imshow("人脸识别系统", image);
            if(waitKey(10) >= 0)//等待10ms，按任意键退出
                break;
        }
    }

    else
    {
        cout << "No capture" << endl;
        image = Mat::zeros(480, 640, CV_8UC1);//创建一个黑色图像
        drawText(image);
        imshow("人脸识别系统", image);//显示一帧图像
        waitKey(0);
    }
    return 0;
}
//在图像上绘制
void drawText(Mat & image)
{
    putText(image, "",
            Point(20, 50),
            FONT_HERSHEY_COMPLEX, 1, // font type and size
            Scalar(255, 255, 255),// font color
            1, LINE_AA); // line thickness and type
            //circle(image, Point(100, 100), 50, Scalar(0, 255, 0), 2); // Draw a circle
}


//图像处理
void processImage(Mat & image)
{
    //遍历矩阵
    for(int i = 0; i < image.rows; i++)
    {
        for(int j = 0; j < image.cols; j++)
        {
            //获取像素点的值
            Vec3b & color = image.at<Vec3b>(i, j);
            //将像素点的值取反
            color[0] = 255 - color[0];
            color[1] = 255 - color[1];
            color[2] = 255 - color[2];
        }
    }
}
