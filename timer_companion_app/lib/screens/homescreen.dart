import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:timer_companion_app/size_config.dart';
import 'package:syncfusion_flutter_gauges/gauges.dart';

class Homescreen extends StatefulWidget {
  const Homescreen({super.key});

  @override
  State<Homescreen> createState() => _HomescreenState();
}

class _HomescreenState extends State<Homescreen> {
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Color.fromARGB(255, 255, 254, 219),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            SizedBox(
              width: double.infinity,
              height: SizeConfig.vertical! * 25,
              child: Column(
                children: [
                  Container(
                    color: const Color.fromARGB(255, 33, 93, 243),
                    height: SizeConfig.vertical! * 12.5,
                    width: double.infinity,
                  ),
                  Container(
                    color: const Color.fromARGB(255, 140, 176, 255),
                    height: SizeConfig.vertical! * 12.5,
                    width: double.infinity,
                    child: Center(
                      child: Text(
                        "Active Timers",
                        textAlign: TextAlign.center,
                        style: GoogleFonts.racingSansOne(fontSize: 30),
                      ),
                    ),
                  ),
                ],
              ),
            ),
            SizedBox(
              width: 300,
              height: 300,
              child: Card(
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(8),
                ),
                color: const Color.fromARGB(255, 59, 75, 255),
                // child: Row(
                //   mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                //   crossAxisAlignment: CrossAxisAlignment.center,
                //   children: [
                //     Text("homework", style: GoogleFonts.onest(fontSize: 20)),
                //     Text("20 min"),
                //   ],
                // ),
                child: SfRadialGauge(
                  axes: <RadialAxis>[
                    RadialAxis(
                      startAngle: 270,
                      endAngle: 270,
                      radiusFactor: 0.8,
                      showLabels: false,
                      showTicks: false,
                      axisLineStyle: const AxisLineStyle(
                        thickness: 1.5,
                        color: Colors.white,
                        cornerStyle:
                            CornerStyle.endCurve, // TODO: round the end
                      ),
                      pointers: <GaugePointer>[
                        RangePointer(
                          value: 50,
                          width: 0.2,
                          color: Colors.green,
                        ),
                      ],
                      annotations: <GaugeAnnotation>[
                        GaugeAnnotation(
                          widget: Column(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              Text(
                                'Homework',
                                style: GoogleFonts.racingSansOne(
                                  fontSize: 27,
                                  color: Colors.white,
                                ),
                              ),
                              Text(
                                '00:20:00',
                                style: GoogleFonts.racingSansOne(
                                  fontSize: 25,
                                  color: Colors.white,
                                ),
                              ),
                            ],
                          ),
                          positionFactor: 0.1,
                          angle: 90,
                        ),
                      ],
                      ranges: [
                        GaugeRange(
                          startValue: 0,
                          endValue: 100,
                          gradient: const SweepGradient(
                            colors: <Color>[
                              Color.fromARGB(255, 234, 255, 0),
                              Color.fromARGB(255, 233, 250, 255),
                            ],
                            stops: <double>[0.0, 1.0],
                          ),
                          startWidth: 17,
                          endWidth: 17,
                        ),
                      ],
                    ),
                    // gradient (orange to purple)
                    // RadialAxis(
                    //   ranges: [
                    //     GaugeRange(
                    //       startValue: 0,
                    //       endValue: 100,
                    //       color: const Color.fromARGB(255, 255, 165, 0),
                    //       startWidth: 10,
                    //       endWidth: 10,
                    //     ),
                    //   ],
                    // ),
                  ],
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(0, 0, 0, 20),
              child: ElevatedButton(
                style: ElevatedButton.styleFrom(
                  side: BorderSide(color: Colors.black, width: 1),
                  minimumSize: const Size(200, 50),
                  backgroundColor: const Color.fromARGB(255, 39, 60, 222),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(8),
                  ),
                ),
                onPressed: () {},
                child: Text(
                  'Connect',
                  style: GoogleFonts.bungee(fontSize: 20, color: Colors.white),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
