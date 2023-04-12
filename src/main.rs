mod mount_tree;

use std::fs;
use std::fs::File;
use std::os::fd::{AsRawFd};
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use crate::mount_tree::*;
use anyhow::{bail, Result};
use sys_mount::{FilesystemType, Mount, MountFlags};

fn mount_ro_overlay(dest: &PathBuf, lower_dirs: &Vec<String>) -> Result<()> {
    let mut mount_seq: Vec<MountTree> = vec![];
    let mut fds: Vec<File> = vec![];
    let tree = MountNode::get_tree()?;
    let tree = MountNode::get_mount_for_path(&tree, &dest).unwrap();
    MountNode::get_top_mounts_under_path(&mut mount_seq, &tree, &dest);
    for mount in &mount_seq {
        let file = File::options()
            .read(true) // NEEDED, otherwise causes EINVAL in Rust (https://github.com/rust-lang/rust/issues/62314)
            .custom_flags(libc::O_PATH)
            .open(&mount.mount_info.mount_point)?;
        println!("{} -> fd {}", mount.mount_info.mount_point.to_str().unwrap(), file.as_raw_fd());
        fds.push(file);
    }
    let mut first = true;
    for (mount, fd) in mount_seq.iter().rev().zip(fds.iter().rev()) {
        let mut lower_count: usize = 0;
        let src: String;
        let mount_point: String;
        let mut overlay_lower_dir = String::from("lowerdir=");
        let stock_is_dir: bool;
        let modified_is_dir: bool;
        if first {
            mount_point = String::from(dest.to_str().unwrap());
            src = String::from(dest.to_str().unwrap());
            dbg!(&mount_point);
            dbg!(&src);
            stock_is_dir = true; // assert
        } else {
            mount_point = String::from(mount.mount_info.mount_point.to_str().unwrap());
            src = format!("/proc/self/fd/{}", fd.as_raw_fd());
            match fd.metadata() {
                Ok(stat) => {
                    stock_is_dir = stat.is_dir();
                }
                Err(e) => bail!(e)
            }
        }
        match fs::metadata(&mount_point) {
            Ok(stat) => {
                modified_is_dir = stat.is_dir();
            }
            Err(e) => {
                match e.raw_os_error().unwrap() {
                    libc::ENOENT | libc::ENOTDIR => {
                        println!("skip");
                        first = false;
                        continue;
                    }
                    _ => {
                        bail!(e);
                    }
                }
            }
        }
        for lower in lower_dirs {
            let lower_dir = format!("{}{}", lower, mount_point);
            dbg!(&lower_dir);
            match fs::metadata(&lower_dir) {
                Ok(stat) => {
                    if !stat.is_dir() && first {
                        println!("{} is an invalid module", lower_dir);
                        continue;
                    }
                    lower_count += 1;
                    if lower_count > 1 {
                        overlay_lower_dir.push_str(":");
                    }
                    overlay_lower_dir.push_str(lower_dir.as_str());
                }
                Err(..) => {
                    continue;
                }
            }
        }
        if lower_count == 0 {
            if first {
                println!("no valid modules, skip");
                break;
            }
            println!("mount bind mount_point={}, src={}", mount_point, src);
            Mount::builder()
                .flags(MountFlags::BIND)
                .mount(src, mount_point)?;
        } else if stock_is_dir && modified_is_dir {
            overlay_lower_dir.push_str(":");
            overlay_lower_dir.push_str(&src);
            println!("mount overlayfs mount_point={}, src={}, options={}", mount_point, src, overlay_lower_dir);
            Mount::builder()
                .fstype(FilesystemType::from("overlay"))
                .data(&overlay_lower_dir)
                .mount("KSU", mount_point)?;
        }
        first = false;
    }
    Ok(())
}

fn umount_ro_overlay() {}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() >= 2 {
        match args[1].as_str() {
            "--tree" => {
                let tree = MountNode::get_tree()?;
                tree.print_tree(0);
            }
            "--mounts" => {
                let path = PathBuf::from(&args[2]);
                let tree = MountNode::get_tree()?;
                let mut sub_mounts: Vec<MountTree> = vec![];
                let mount = MountNode::get_mount_for_path(&tree, &path).expect("");
                MountNode::get_top_mounts_under_path(&mut sub_mounts, &mount, &path);
                for mnt in sub_mounts {
                    mnt.print();
                }
            }
            "--test" => {
                match fs::metadata(PathBuf::from(&args[2])) {
                    Ok(..) => {
                        println!("exists");
                    }
                    Err(e) => match e.raw_os_error().unwrap() {
                        libc::ENOENT | libc::ENOTDIR => {
                            println!("not exists");
                        }
                        _ => {
                            bail!(e);
                        }
                    }
                }
            }
            _ => {
                let mut lower_dirs: Vec<String> = vec![];
                let path = PathBuf::from(&args[1]);
                for i in 2..args.len() {
                    lower_dirs.push(String::from(&args[i]));
                }
                mount_ro_overlay(&path, &lower_dirs).expect("failed to mount");
            }
        }
    }
    Ok(())
}
