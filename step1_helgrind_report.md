# Helgrind Report — Step 1

## Mục tiêu

Kiểm tra các thread của `sensor_gateway` khi khởi động và dừng theo yêu cầu của Step 1. Helgrind được dùng để phát hiện các truy cập dữ liệu không được đồng bộ giữa các thread.

## Cách chạy

Hai phiên bản được chạy với cổng gốc `5000` và cổng HTTP `8080`. Sau khi ba manager thread khởi động, gửi `SIGTERM` để kiểm tra quá trình dừng có kiểm soát.

Các lệnh build và chạy hai phiên bản:

```sh
# Bản baseline trước mutex, được dựng riêng trong /tmp
gcc -std=c11 -Wall -Wextra -pthread -DDEBUG \
    /tmp/step1_helgrind_before/main.c \
    -o /tmp/step1_helgrind_before/server
ulimit -n 1024
valgrind --tool=helgrind --error-exitcode=99 \
    /tmp/step1_helgrind_before/server 5000 8080

# Bản hiện tại sau mutex
make debug
ulimit -n 1024
valgrind --tool=helgrind --error-exitcode=99 ./server 5000 8080
```

Trong mỗi lần chạy, gửi `SIGTERM` tới tiến trình Valgrind sau khi các thread đã khởi động để kiểm tra shutdown.

Giới hạn file descriptor được hạ xuống `1024` vì Valgrind trong môi trường này không khởi động được với giới hạn mặc định rất cao. `SIGTERM` được gửi trong lúc chương trình đang chạy để kết thúc phép kiểm tra.

## So sánh kết quả

| Phiên bản | Cách chia sẻ cờ dừng | Kết quả Helgrind | Exit code |
|---|---|---:|---:|
| Trước mutex | Signal handler ghi `gateway.stop`; các manager thread đọc cờ này khi polling | `4 errors from 2 contexts` | `99` |
| Sau mutex | Main thread đặt `workers_stop` dưới mutex rồi broadcast condition variable; worker đọc cờ dưới cùng mutex | `0 errors from 0 contexts` | `0` |

### Output tiêu biểu trước mutex

```text
Possible data race during write ... by thread #1
  at handle_termination_signal
This conflicts with a previous read ... by thread #4
  at manager_thread
...
Possible data race during read ... by thread #2
  at manager_thread
This conflicts with a previous write ... by thread #1
  at handle_termination_signal
...
ERROR SUMMARY: 4 errors from 2 contexts (suppressed: 91 from 27)
before_mutex_helgrind_exit_code=99
```

### Output tiêu biểu sau mutex

```text
Connection manager thread started
Data manager thread started
Storage manager thread started
Connection manager thread stopped
Storage manager thread stopped
Data manager thread stopped
Gateway started …
Gateway shutting down …
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 89 from 35)
after_mutex_helgrind_exit_code=0
```

Lần chạy trước mutex sử dụng một bản baseline tạm trong `/tmp`, tái tạo logic Step 1 trước khi thêm mutex: signal handler cập nhật cờ `volatile sig_atomic_t`, còn các thread polling và đọc cùng cờ. Bản chạy sau mutex được build và chạy trực tiếp từ code hiện tại trong workspace.

## Diễn giải

Trước khi sửa, signal handler và worker thread truy cập cùng một cờ mà không có đồng bộ giữa các thread. `volatile sig_atomic_t` đáp ứng yêu cầu truy cập cờ từ signal handler, nhưng không đồng bộ truy cập giữa nhiều thread. Helgrind báo xung đột ghi/đọc ở `gateway.stop`; `--error-exitcode=99` khiến Valgrind trả exit code `99` khi phát hiện lỗi.

Code hiện tại giữ `gateway.stop` cho signal handler và main thread. Sau khi nhận signal, main thread đặt `workers_stop` dưới `stop_lock` rồi broadcast `stop_cond`. Các manager thread chờ trên condition variable và kiểm tra `workers_stop` khi giữ cùng mutex. Điều này đồng bộ hóa việc đọc/ghi cờ worker và Helgrind không báo lỗi trong lần chạy sau sửa đổi. Các mục `suppressed` là báo cáo đã được Valgrind bỏ qua theo suppression rules; chúng không được tính vào số lỗi.

## Phạm vi kết luận

Kết quả này xác nhận không có lỗi Helgrind được phát hiện trong đường chạy đã kiểm tra: khởi tạo ba thread, chờ `SIGTERM`, đánh thức thread và join chúng. Nó không xác nhận các đường chạy hoặc lỗi khác chưa được thực thi.
