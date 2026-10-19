# 手势训练数据

原始实机采集记录不随公开版本分发。已有模型保留在根目录的 gesture_model.h 和 gesture_model.npz。
重新训练时，把 CSV 放入 data/1、data/2 等分类目录，然后运行 train_gesture.py。
CSV 列为 timestamp_ms,acc_x,acc_y,acc_z,gyr_x,gyr_y,gyr_z,label。
