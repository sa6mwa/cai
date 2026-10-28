# Test-data inspection only. Production restoration streams bytes directly.
def byteprefix($limit):
  explode | reduce .[] as $cp ({bytes:0, text:[]};
    if .bytes < $limit then
      .bytes += (if $cp < 128 then 1 elif $cp < 2048 then 2 elif $cp < 65536 then 3 else 4 end) |
      .text += [$cp]
    else . end) | .text | implode;
def cai_history:
  reduce (.[] | select(.record_type == "commit") | .state) as $edit
    (""; byteprefix($edit.base_bytes) + $edit.history_append) |
  split("\n") | map(select(startswith("[")) | fromjson) | add // [];
