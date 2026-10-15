// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Properties;

/** Crash-recoverable write-permission lease; never changes node ownership. */
public final class BacklightLease {
    public interface Access {
        String get(File file) throws IOException;
        void set(File file,String mode) throws IOException;
    }
    public static final Access POSIX=new Access() {
        public String get(File f)throws IOException{return PosixFilePermissions.toString(Files.getPosixFilePermissions(f.toPath()));}
        public void set(File f,String mode)throws IOException{Files.setPosixFilePermissions(f.toPath(),PosixFilePermissions.fromString(mode));}
    };
    private final File root,node,journal;
    private final String boot;
    private final Access access;
    private boolean active=false;
    public BacklightLease(File root,String name,File journal,String boot,Access access) {
        if(!name.matches("[A-Za-z0-9_-]+"))throw new IllegalArgumentException("invalid backlight node");
        this.root=root;this.node=new File(new File(root,name),"brightness");this.journal=journal;this.boot=boot;this.access=access;
    }
    public boolean active(){return active;}
    public boolean intact()throws IOException{return active&&access.get(node).equals("r--r--r--");}
    public void acquire()throws IOException {
        if(active){if(!intact())throw new IOException("backlight permissions changed externally");return;}
        if(journal.exists())throw new IOException("unrecovered backlight permission lease");
        String original=access.get(node);
        // Do not take over nodes already locked by someone else, or special modes.
        if(!original.equals("rw-r--r--")&&!original.equals("rw-rw-r--")&&!original.equals("rw-rw----"))
            throw new IOException("unsupported original backlight permissions: "+original);
        Properties p=new Properties();p.setProperty("node",node.getParentFile().getName());
        p.setProperty("boot",boot);p.setProperty("original",original);
        File tmp=File.createTempFile(".permissions-",".tmp",journal.getParentFile());
        try {
            try(FileOutputStream out=new FileOutputStream(tmp)){p.store(out,"IAB backlight permission recovery");out.getFD().sync();}
            Files.move(tmp.toPath(),journal.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{tmp.delete();}
        // Recovery is durable before chmod, including failure between these operations.
        active=true;
        try{access.set(node,"r--r--r--");if(!intact())throw new IOException("backlight permission lease failed");}
        catch(IOException|RuntimeException e){release();throw e;}
    }
    public void release()throws IOException {
        restore(root,journal,boot,access);active=false;
    }
    public static void restore(File root,File journal,String boot,Access access)throws IOException {
        if(!journal.isFile())return;
        Properties p=new Properties();try(InputStream in=new FileInputStream(journal)){p.load(in);}
        if(!boot.equals(p.getProperty("boot"))){Files.delete(journal.toPath());return;}
        String name=p.getProperty("node",""),original=p.getProperty("original","");
        if(!name.matches("[A-Za-z0-9_-]+")||(!original.equals("rw-r--r--")&&!original.equals("rw-rw-r--")&&!original.equals("rw-rw----")))
            throw new IOException("invalid backlight permission recovery data");
        File node=new File(new File(root,name),"brightness");
        // A permission change made by another owner wins. Keep the journal on I/O
        // failure so supervisor/startup recovery can retry instead of losing it.
        if(access.get(node).equals("r--r--r--"))access.set(node,original);
        Files.delete(journal.toPath());
    }
}
