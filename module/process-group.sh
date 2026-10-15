#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Library: detach only this root launcher, before it forks long-lived children.
# setsid changes the Unix session, not Android's app cgroup/phantom ownership.
iab_cgroup_membership() { cat "/proc/$$/cgroup"; }
iab_cgroup_targets() {
  awk -v groups="$1" '
    BEGIN {
      n=split(groups, lines, "\n"); count=0;
      for(i=1;i<=n;i++) {
        if(lines[i]=="") continue;
        if(split(lines[i], f, ":")!=3 || f[3]!~/^\//) {bad=1; continue}
        count++;
        if(f[3] ~ /\/(uid_[0-9]+|pid_[0-9]+)(\/|$)/) wanted[f[1]]=f[2];
      }
      if(!count) bad=1;
    }
    {
      sep=0; for(i=7;i<=NF;i++) if($i=="-") {sep=i; break}
      if(!sep || $4!="/" || $5!~/^\// || $5~/\\/) next;
      for(id in wanted) {
        match_group=0;
        if(wanted[id]=="") match_group=($(sep+1)=="cgroup2");
        else if($(sep+1)=="cgroup") {
          k=split(wanted[id], controllers, ","); match_group=1;
          for(j=1;j<=k;j++) if(index("," $(sep+3) ",", "," controllers[j] ",")==0) match_group=0;
        }
        if(match_group && !(id in target)) target[id]=$5 "/cgroup.procs";
      }
    }
    END {
      for(id in wanted) if(!(id in target)) bad=1;
      if(bad) exit 1;
      for(id in target) print target[id];
    }' "/proc/$$/mountinfo"
}
iab_cgroup_write() { printf '%s\n' "$$" > "$1"; }
iab_detach_cgroups() {
  IAB_CGROUP_MOVED=0
  iab_groups=$(iab_cgroup_membership) || return 1
  iab_targets=$(iab_cgroup_targets "$iab_groups") || {
    echo 'Cannot resolve app cgroup roots; refusing dependent service launch' >&2
    return 1
  }
  # mountinfo paths containing escapes/whitespace were excluded above.
  for iab_target in $iab_targets; do
    [ -f "$iab_target" ] && iab_cgroup_write "$iab_target" || {
      echo "Cannot leave app cgroup via $iab_target" >&2
      return 1
    }
    IAB_CGROUP_MOVED=1
  done
  iab_groups=$(iab_cgroup_membership) || return 1
  iab_remaining=$(iab_cgroup_targets "$iab_groups") || return 1
  [ -z "$iab_remaining" ] || {
    echo 'App cgroup membership remains; refusing dependent service launch' >&2
    return 1
  }
}
