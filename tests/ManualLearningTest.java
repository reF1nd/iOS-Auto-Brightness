// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.ManualLearning;
public final class ManualLearningTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args) {
        var m=new ManualLearning();
        m.change(1000,100000,10);check(!m.sample(4000,true,.1f),"unarmed settings changes never teach");
        check(!m.settingChanged("1","2"),"unarmed polling cannot infer intent");
        m.arm(5000);check(m.settingChanged("1","2")&&!m.settingChanged(null,"2")&&!m.settingChanged("2","2"),"armed delta requires a real baseline");check(!m.sample(8000,true,.1f),"arming alone never teaches module output");
        m.change(9000,109000,20);check(!m.sample(9000,true,.2f),"wait for actual output");
        check(!m.sample(10000,true,.3f),"moving output resets stability");
        check(!m.sample(11499,true,.3f),"full stability window");
        check(m.sample(11500,true,.3f)&&m.value==.3f&&m.lux==20,"normalized settled value");
        check(!m.sample(11501,false,.1f)&&m.until==0,"thermal or state change cancels pending learning");
        m.arm(20000);m.change(21000,121000,20);check(!m.sample(21000,true,Float.NaN)&&m.until==0,"invalid output cancels");
        m.arm(30000);m.change(61000,161000,20);check(!m.sample(61000,true,.2f)&&m.until==0,"expired intent ignored");
        m.arm(70000);m.change(71000,171000,20);check(!m.sample(82000,true,.2f)&&m.until==0,"never save delayed unrelated output");
        System.out.println("Explicit fallback intent, debounce, expiry and protection cancellation passed");
    }
}
