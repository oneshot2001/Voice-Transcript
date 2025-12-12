python -m wyoming_piper \
  --uri tcp://0.0.0.0:10200 \
  --data-dir /home/fred/assistant/wyoming/data \
  --voice sv_SE-nst-medium

python -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/wyoming \
  --model large-v3 \
  --language sv \
  --device cuda \
  --compute-type float16 \
  --beam-size 1