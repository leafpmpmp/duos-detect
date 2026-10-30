# duos-detect

Person detection for the [Milk-V Duo S](https://milkv.io/duo-s) on mainline
Linux. The program reads the Raspberry Pi Camera Module 3 and runs a YOLO11
detector on the SG2000 TPU. It sends the results and a live preview over
HTTP. It runs on the ARM core or on the RISC-V core.

- Detection at 60 fps, with 35 ms from frame capture to result.
- INT8 YOLO11 person models, fine-tuned on COCO.
- JSON API, Server-Sent Events and an MJPEG preview with boxes.
- Contrast autofocus.
- Debian packages and a systemd service.

## Performance

Live camera, real-time priority (`-R 50`), statistics over 20 to 60 s. Latency is
the time from the capture timestamp of a frame to its detection result.

| Model | Core | Sensor mode | fps | Front end | TPU | Latency mean / max | CPU load |
|---|---|---|---|---|---|---|---|
| person-yolo11n-448x320 | ARM A53 | 1728x1296, 60 fps | 60.0 | 6.1 ms | 11.9 ms | 33 / 34 ms | 40% |
| person-yolo11n-448x320 | RISC-V C906 | 1728x1296, 60 fps | 59.9 | 8.0 ms | 12.0 ms | 35 / 41 ms | 51% |
| person-yolo11n-448x320 | RISC-V C906 | 2304x1296, 56 fps | 56.0 | 9.6 ms | 12.3 ms | 40 / 42 ms | 59% |
| person-yolo11n-576x320 | ARM A53 | 2304x1296, 56 fps | 55.8 | 9.0 ms | 15.3 ms | 43 / 45 ms | 54% |
| person-yolo11n-576x320 | RISC-V C906 | 2304x1296, 56 fps | 55.8 | 12.6 ms | 15.4 ms | 46 / 56 ms | 74% |
| person-yolo11s-448x320 | ARM A53 | 1728x1296, 30 fps | 29.9 | 6.2 ms | 33.2 ms | 55 / 57 ms | 21% |
| person-yolo11s-448x320 | RISC-V C906 | 1728x1296, 30 fps | 30.0 | 8.1 ms | 33.2 ms | 57 / 62 ms | 27% |

The 1728x1296 mode goes up to 68 fps (ARM: 68.4 fps at 48% CPU load; RISC-V:
68.4 fps at 62%).

On RISC-V, the front end and the preview use the XTheadVector extension when
the kernel enables it. Without it, they use scalar code: the front end then
takes 14.0 ms instead of 7.3 ms per 1728x1296 frame.

## Accuracy

AP50 of the person class, INT8 on the board. Each test image is letterboxed
into the detector input.

| Model | COCO val2017 | VOC2007 test | CrowdHuman val (visible box) | MOT17 train |
|---|---|---|---|---|
| person-yolo11n-448x320 | 0.710 | 0.889 | 0.562 | 0.494 |
| person-yolo11n-576x320 | 0.719 | 0.888 | 0.588 | 0.585 |
| person-yolo11s-448x320 | 0.763 | 0.913 | 0.607 | 0.523 |

- `person-yolo11n-448x320`: the fastest model. The service uses it by default.
- `person-yolo11n-576x320`: the full sensor width. Better for small, distant
  people (MOT17).
- `person-yolo11s-448x320`: the most accurate model, about 30 fps. Use it with
  `-r 30`.

## Requirements

Hardware:

- Milk-V Duo S (SG2000). Use the ARM core (Cortex-A53) or the RISC-V core
  (C906). The boot switch selects the core.
- Raspberry Pi Camera Module 3 (IMX708) on connector J2. Use a short 15-pin
  cable with 1 mm pitch. A 150 mm cable works. A 500 mm cable gave I2C errors.

Software on the board:

- [Armbian](https://github.com/armbian/build) for the Milk-V Duo S (boards
  `milkv-duos-riscv` and `milkv-duos-arm`, branch `edge`). The image includes
  the TPU driver (`/dev/cvi-tpu0`) and the camera drivers from
  [sg200x-camera-dkms](https://github.com/leafpmpmp/sg200x-camera-dkms).
- The J2 camera overlay. In `/boot/armbianEnv.txt`, add `camera-j2-imx708`
  to the `overlays=` line, then reboot.
- sg200x-camera-dkms commit `4db02e6` or later. In case Armbian pins an older commit,
  build the image with
  `SG200X_CAMERA_REF=commit:4db02e63ee1ca7b8a92889bea53ac44351fe2c3d`.
  The older driver has two problems:
  - It has no 1728x1296 mode. Every model then uses the 2304x1296 mode, up to
    56 fps.
  - On the ARM core, it loses every second frame at 56 fps, so you get 28 fps.
    Add `-r 54` to the options. 54 fps works without frame loss.
- For real-time priority (`-R`): the kernel parameter `rt_group_sched=0`. In
  `/boot/armbianEnv.txt`, add `extraargs=rt_group_sched=0`. Armbian kernels
  have `CONFIG_RT_GROUP_SCHED=y`, and without this parameter the kernel
  refuses real-time priority to services and to SSH sessions. The program
  then runs at normal priority. It can drop frames when other programs use
  the CPU.
- For the vector code on RISC-V: a kernel that enables XTheadVector. To check,
  run `grep -o xtheadvector /proc/cpuinfo`.

## Install

The TPU runtime (cviruntime) has no licence that permits redistribution.
Thus you must build its package yourself (see [Build](#build)). Then copy both
packages to the board and install them (use `arm64` on the ARM core):

```sh
apt install ./cviruntime_1.4.0-2_riscv64.deb ./duos-detect_0.2.0_riscv64.deb
```

Start the detector and open `http://<board>:8080/` in a browser:

```sh
duos-detect -m /usr/share/duos-detect/models/person-yolo11n-448x320.cvimodel -H 8080 -f auto
```

To run it as a service, also at every boot: (Not recommended, as this may overload the single core CPU and cause usability issues.)

```sh
systemctl enable --now duos-detect
```

- The options are in `/etc/default/duos-detect`. After a change, run
  `systemctl restart duos-detect`.
- `journalctl -u duos-detect -f` shows the statistics once a minute.
- The service holds the camera. Stop it before you start `duos-detect` by
  hand: `systemctl stop duos-detect`.

## Usage

```
usage: duos-detect -m model.cvimodel [options]
  -d dev     capture device (default: found in the media graph)
  -s WxH     sensor mode (default: the best fit for the model input)
  -r fps     frame rate (default: the fastest of the mode)
  -K         keep the camera setup as it is (no flips, modes, rate)
  -i file    detect on planar RGB images instead of the camera
             ("-": a stream of them on stdin)
  -n frames  stop after this many frames (0: until Ctrl-C)
  -c conf    score threshold (0.4)
  -I iou     NMS overlap threshold (0.45)
  -M boxes   most boxes per frame (64, at most 300)
  -k class   class index (0: person)
  -g R,G,B   white balance gains (1.942,1,1.623)
  -B black   black level, 8-bit scale (16)
  -p         print boxes as JSON lines
  -L sec     statistics every sec seconds (2; 0: only at the end)
  -S         sequential: no CPU/TPU overlap
  -f pos     focus lens position (Camera Module 3: 0..1023), or
             "auto": contrast autofocus at start
  -H port    serve a preview and the results over HTTP
  -F fps     preview frame rate (15)
  -R prio    real-time: TPU thread SCHED_FIFO at prio, capture at
             prio-1, the TPU driver's worker at prio+1
```

The program sets up the camera itself. It finds the sensor in the media
graph, flips the image when the sensor is mounted upside down, and selects
the sensor mode that fits the model input best:

| Sensor mode | Field of view | Max fps | Used by |
|---|---|---|---|
| 2304x1296 | full | 56 | 576x320 model (full width); 448x320 models when the 4:3 mode is missing (centre 78%) |
| 1728x1296 (4:3) | 75% of the width, full height | 68 | 448x320 models |

Each statistics line looks like this:

```
 56.0 fps | front end 9.59 ms | tpu 12.28 ms | decode 0.02 ms | latency 40.1 ms (max 44.6) | dropped 0 | lost 0 | cpu 59% | peak front end 10.8 tpu 12.7 ms
```

- `dropped` counts the frames that a newer frame replaced before the TPU took
  them. The program never queues old frames: this keeps the latency low.
- `lost` counts the frames that the camera driver lost before the program got
  them. When the driver loses more than 10% of the frames, the program prints
  a warning. Then set a lower frame rate with `-r`.

## HTTP API

Start the program with `-H port`. All requests are `GET`.

| Path | Result |
|---|---|
| `/` | Viewer page: the live preview with boxes, a focus slider and an autofocus button |
| `/stream.mjpg` | MJPEG stream with boxes, at the `-F` frame rate |
| `/snapshot.jpg` | The next frame as one JPEG |
| `/api/detections` | The latest result as JSON |
| `/api/events` | Server-Sent Events: one JSON result per frame |
| `/api/focus` | Lens state. `?value=N` moves the lens, `?auto` starts autofocus |

A result:

```json
{"seq":1234,"latency_ms":40.1,"frame":[2304,1296],"boxes":[[812.0,301.5,1033.2,1180.0,0.8731]]}
```

`frame` is the sensor mode. Each box is `[x1, y1, x2, y2, score]`, in pixels
of the sensor frame. To follow the results from another computer:

```sh
curl -N http://<board>:8080/api/events
```

The preview uses only spare CPU time (`SCHED_IDLE`), so it does not slow the
detection. On RISC-V at 56 fps detection, the preview gets about 6 fps.

## How it works

1. The IMX708 sends RAW12 frames over two CSI-2 lanes. The VI writes them to
   cached V4L2 buffers.
2. The CPU front end turns each 4x4 Bayer block into one RGB pixel. One
   lookup table applies the black level, the white balance and the gamma. The
   front end writes the INT8 detector input directly into TPU memory. It uses
   NEON on ARM and XTheadVector on RISC-V.
3. The TPU runs the INT8 network up to the head convolutions. The CPU decodes
   the boxes and applies the score threshold and NMS.
4. The CPU prepares frame N+1 while the TPU runs frame N.

## Build

You need a Linux PC (x86-64) with Docker and git. The scripts cross-compile
in a Debian container (`docker/Dockerfile`).

1. Build the TPU runtime. It comes from the vendor SDK, with a port to the
   mainline TPU driver. A sparse clone gets only the parts the build needs:

   ```sh
   git clone --depth 1 --filter=blob:none --sparse -b mainline-tpu \
       https://github.com/leafpmpmp/duo-buildroot-sdk-v2 ../duo-sdk
   git -C ../duo-sdk sparse-checkout set cviruntime cvikernel cvibuilder flatbuffers cnpy
   tools/build-cviruntime.sh riscv64 ../duo-sdk
   ```

   The runtime goes to `build/cviruntime/riscv64/install`.

2. Build the program:

   ```sh
   ./build.sh riscv64
   ```

   The output goes to `build/riscv64-linux-gnu/`: `duos-detect`, plus the
   benchmarks `fe-bench` and `jpeg-bench`.

3. Get the models from the
   [releases](https://github.com/leafpmpmp/duos-detect/releases) and put them
   in `work/models/`. Then build both packages:

   ```sh
   packaging/build-deb.sh riscv64
   ```

   The packages go to `work/pkg/`.

For the ARM core, use `arm64` in each step.

## Train a model

The scripts in `train/` fine-tune an Ultralytics YOLO model on the person
class of COCO and compile it to an INT8 cvimodel. They expect a Python
environment in `work/.venv-ultra` and the TPU-MLIR container image
`tpu-mlir:1.30.2`.

1. Make the Python environment (a CUDA GPU makes training practical):

   ```sh
   python3 -m venv work/.venv-ultra
   work/.venv-ultra/bin/pip install ultralytics==8.4.183 onnx onnxslim onnxruntime-gpu pycocotools
   ```

2. Make the TPU-MLIR image:

   ```sh
   printf 'FROM sophgo/tpuc_dev:latest\nRUN pip install "tpu_mlir[onnx]==1.30.2"\n' |
       docker build -t tpu-mlir:1.30.2 -
   ```

3. Get the dataset (12 GB of COCO images):

   ```sh
   train/coco_person.sh
   ```

4. Train. The released models trained for 60 epochs at 480 pixels. For
   `yolo11s` on a GPU with 8 GB, add `--batch 32`:

   ```sh
   work/.venv-ultra/bin/python train/train.py yolo11n-silu --base yolo11n \
       --data work/datasets/coco-person/coco-person.yaml
   ```

5. Make the INT8 calibration set at the detector size. `--camera DIR` adds
   the camera images in DIR to the COCO images:

   ```sh
   work/.venv-ultra/bin/python train/make_calib.py work/calib/person448 --size 448x320 --n 400 \
       --coco-list work/datasets/coco-person/train2017.txt --coco-root work/datasets/coco-person
   ```

6. Compile:

   ```sh
   train/deploy.sh yolo11n-silu 448 320
   ```

   The model goes to `work/deploy/yolo11n-silu-448x320.cvimodel`.

`train/eval_all.sh` measures the AP of a model on the four test sets, float
on the PC and INT8 on the board. `tools/datasets.py` converts VOC2007,
CrowdHuman and MOT17 to COCO-format ground truth. These test sets have their
own licences, and some permit only non-commercial use.

## Repository

| Path | Contents |
|---|---|
| `src/` | The program |
| `tools/` | Benchmarks, evaluation, the TPU runtime build, camera scripts, Python reference front ends |
| `train/` | Dataset, training, INT8 compilation and evaluation scripts |
| `packaging/` | Debian packages and the systemd service |
| `docker/` | The cross-build image |
| `spikes/` | Early experiments: zero-copy camera-to-TPU test, CPU front end prototype |

## Limits

- The models find people only. For other classes, train a model and use
  `-k`.
- There is no auto exposure. Set the exposure and the analogue gain with
  `v4l2-ctl` on the sensor subdevice.
- The white balance gains are fixed (`-g`). The defaults suit indoor light.
- The preview is MJPEG, encoded on the CPU. There is no H.264 or RTSP output.
- The only supported sensor is the IMX708.

## Licence

duos-detect is free software under the GNU Affero General Public License,
version 3 or later. See [LICENSE](LICENSE).

The models are fine-tuned from Ultralytics YOLO11 weights (AGPL-3.0) on the
COCO 2017 annotations (CC BY 4.0).

The TPU runtime is not part of this repository. Its sources are copyright
Cvitek and Bitmain and carry no licence grant. Build its package for your own
boards. Do not redistribute it.
